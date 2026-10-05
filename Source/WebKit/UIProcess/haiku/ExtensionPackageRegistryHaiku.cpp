/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "ExtensionPackageRegistryHaiku.h"
#include "WebExtensionTimingHaiku.h"
#include "haiku/ExtensionActionPopupHaiku.h"

#if ENABLE(WK_WEB_EXTENSIONS)
#include "APIError.h"
#include "APIPageConfiguration.h"
#include "BrowserTabRegistryHaiku.h"
#include "WebExtensionAction.h"
#include "WebExtensionTab.h"
#include "WebExtensionMatchPattern.h"
#include "WebExtensionControllerConfiguration.h"
#include "haiku/WebExtensionStateHaiku.h"
#include "WebKitContext.h"
#include <WebCore/GraphicsContext.h>
#include <WebCore/Icon.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/PixelBuffer.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>
#include <wtf/RunLoop.h>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/UUID.h>

namespace WebKit {

// Permissions plus origins one installation may grant.
static constexpr size_t maximumExtensionGrants = 4096;

std::shared_ptr<ExtensionPackageRegistryHaiku> ExtensionPackageRegistryHaiku::create(Ref<WebExtensionController>&& controller, bool privateBrowsing)
{
    ASSERT(RunLoop::isMain());
    return std::shared_ptr<ExtensionPackageRegistryHaiku>(new ExtensionPackageRegistryHaiku(WTF::move(controller), privateBrowsing));
}

ExtensionPackageRegistryHaiku::ExtensionPackageRegistryHaiku(Ref<WebExtensionController>&& controller, bool privateBrowsing)
    : m_controller(WTF::move(controller))
    , m_privateBrowsing(privateBrowsing)
    , m_queue(WorkQueue::create("WebKit.ExtensionPackages"_s, WorkQueue::QOS::Utility))
{
}

ExtensionPackageRegistryHaiku::~ExtensionPackageRegistryHaiku()
{
    shutdown();
}

status_t ExtensionPackageRegistryHaiku::reply(const BMessenger& target, uint32 what, uint64_t identifier, status_t error, BMessage* details)
{
    BMessage message = details ? *details : BMessage();
    message.what = what;
    if (auto status = message.AddUInt64("identifier", identifier); status != B_OK)
        return status;
    if (auto status = message.AddInt32("error", error); status != B_OK)
        return status;
    return target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
}

void ExtensionPackageRegistryHaiku::prepare(String&& path, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    status_t error = B_OK;
    if (m_shutdown)
        error = B_CANCELED;
    else if (!identifier || identifier == std::numeric_limits<uint64_t>::max() || path.isEmpty() || !path.startsWith('/') || path.contains('\0'))
        error = B_BAD_VALUE;
    else if (m_requests.contains(identifier))
        error = B_NAME_IN_USE;
    else if (m_inFlight + m_prepared.size() >= 8)
        error = B_BUSY;
    if (error != B_OK) {
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, error);
        return;
    }
    auto operation = ++m_nextOperation;
    m_requests.add(identifier, Request { target, operation });
    ++m_inFlight;
    SUMMIT_EXTENSION_TIMING("prepare %s queued", path.utf8().legacyCStringPointer());
    m_queue->dispatch([weakThis = weak_from_this(), path = path.isolatedCopy(), identifier, operation] {
        auto prepared = prepareWebExtensionPackageHaiku(path);
        SUMMIT_EXTENSION_TIMING("prepare %s %s", path.utf8().legacyCStringPointer(), prepared ? "unpacked" : "failed");
        std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> isolated = prepared
            ? std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> { WTF::move(*prepared).isolatedCopy() }
            : std::unexpected(prepared.error());
        RunLoop::mainSingleton().dispatch([weakThis, identifier, operation, prepared = WTF::move(isolated)]() mutable {
            if (auto registry = weakThis.lock())
                registry->complete(identifier, operation, WTF::move(prepared));
        });
    });
}

void ExtensionPackageRegistryHaiku::complete(uint64_t identifier, uint64_t operation, std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku>&& prepared)
{
    ASSERT(RunLoop::isMain());
    ASSERT(m_inFlight);
    --m_inFlight;
    auto request = m_requests.find(identifier);
    if (request == m_requests.end() || request->value.operation != operation)
        return;
    auto target = request->value.target;
    m_requests.remove(request);
    if (!target.IsValid())
        return;
    if (!prepared) {
        auto error = prepared.error() == WebExtensionPackageErrorHaiku::InvalidPath ? B_ENTRY_NOT_FOUND : B_BAD_DATA;
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, error);
        return;
    }
    RefPtr<API::Error> parseError;
    Ref extension = WebExtension::create(WTF::move(*prepared), parseError);
    SUMMIT_EXTENSION_TIMING("prepare parsed manifest of %s", extension->displayName().utf8().legacyCStringPointer());
    if (parseError) {
        BMessage details;
        details.AddString("description", parseError->localizedDescription().utf8().legacyCStringPointer());
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, B_BAD_DATA, &details);
        return;
    }
    auto token = WTF::UUID::createVersion4().toString();
    BMessage details;
    status_t error = B_OK;
    auto add = [&](const char* key, const String& value) {
        if (error == B_OK)
            error = details.AddString(key, value.utf8().legacyCStringPointer());
    };
    add("token", token);
    add("name", extension->displayName());
    add("version", extension->version());
    add("description", extension->displayDescription());
    add("fingerprint", extension->nativeResourceFingerprint());
    add("verified_crx_id", extension->nativeVerifiedCRXIdentifier());
    // Keep the complete parsed manifest available to the installer. WebKit's
    // permission sets omit names it does not recognize; that omission must not
    // hide a package requirement from the host's compatibility/consent UI.
    add("manifest_json", extension->manifestObject()->toJSONString());
    Vector<String> permissions, origins;
    for (auto& permission : extension->requestedPermissions())
        permissions.append(permission);
    // Leave out patterns a broader requested pattern already covers: Privacy
    // Badger's content scripts name 390 Google and Facebook hosts next to
    // <all_urls>, which would otherwise be listed (and granted) one by one.
    auto patterns = copyToVector(extension->allRequestedMatchPatterns());
    for (auto& pattern : patterns) {
        bool covered = std::ranges::any_of(patterns, [&](auto& other) {
            return other.ptr() != pattern.ptr() && other->matchesPattern(pattern) && !pattern->matchesPattern(other);
        });
        if (!covered)
            origins.append(pattern->string());
    }
    std::sort(permissions.begin(), permissions.end(), WTF::codePointCompareLessThan);
    std::sort(origins.begin(), origins.end(), WTF::codePointCompareLessThan);
    for (auto& permission : permissions)
        add("permission", permission);
    for (auto& origin : origins)
        add("origin", origin);
    if (error == B_OK)
        error = details.AddDouble("manifest_version", extension->manifestVersion());
    if (error != B_OK) {
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, error);
        return;
    }
    // Retain exactly the object described by the reply. No extension code runs
    // during preparation, and caller edits cannot alter the retained snapshot.
    m_prepared.add(token, WTF::move(extension));
    if (auto delivery = reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, B_OK, &details); delivery != B_OK) {
        m_prepared.remove(token);
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, delivery);
    }
}

void ExtensionPackageRegistryHaiku::cancel(uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    if (!identifier || identifier == std::numeric_limits<uint64_t>::max())
        return;
    auto request = m_requests.find(identifier);
    if (request == m_requests.end())
        return;
    auto target = request->value.target;
    m_requests.remove(request);
    reply(target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, identifier, B_CANCELED);
}

void ExtensionPackageRegistryHaiku::discard(const String& token, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    bool removed = !token.isEmpty() && m_prepared.remove(token);
    if (target.IsValid())
        reply(target, B_WEBKIT_EXTENSION_PACKAGE_DISCARDED, identifier, removed ? B_OK : B_NAME_NOT_FOUND);
}

void ExtensionPackageRegistryHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    m_shutdown = true;
    m_actionListener = { };
    auto pending = std::exchange(m_requests, { });
    for (auto& request : pending)
        reply(request.value.target, B_WEBKIT_EXTENSION_PACKAGE_PREPARED, request.key, B_CANCELED);
    auto loaded = std::exchange(m_loaded, { });
    for (auto& entry : loaded) {
        entry.value->setNativeActionChangeHandler(nullptr);
        if (entry.value->isLoaded())
            m_controller->unload(entry.value.get());
    }
    m_prepared.clear();
}

static bool validExtensionIdentity(const std::string& identifier)
{
    if (identifier.empty() || identifier.size() > 255 || identifier == "." || identifier == "..")
        return false;
    // This identity is also a storage-directory component. Support Chrome IDs,
    // Safari bundle identifiers, and Firefox email/brace UUID identifiers.
    return std::ranges::all_of(identifier, [](unsigned char c) {
        return WTF::isASCIIAlphanumeric(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '{' || c == '}';
    });
}

void ExtensionPackageRegistryHaiku::load(const String& token, const BWebKitExtensionLoadOptions& options, const BWebKitExtensionInstallation& installation, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    BMessage details;
    auto uniqueIdentifier = String::fromUTF8(options.uniqueIdentifier);
    auto respond = [&](status_t error, RefPtr<API::Error> reason = nullptr) {
        if (reason)
            details.AddString("description", reason->localizedDescription().utf8().legacyCStringPointer());
        return reply(target, B_WEBKIT_EXTENSION_LOADED, identifier, error, &details);
    };
    auto refuse = [&](const char* description) {
        details.AddString("description", description);
        return reply(target, B_WEBKIT_EXTENSION_LOADED, identifier, B_NOT_ALLOWED, &details);
    };
    if (installation.identifier.empty() != !installation.order
        || (!installation.identifier.empty() && !validExtensionIdentity(installation.identifier))) {
        respond(B_BAD_VALUE);
        return;
    }
    if (installation.order) {
        for (const auto& loaded : m_loaded) {
            if (loaded.value->nativeInstallationOrder() == installation.order
                || loaded.value->nativeInstallationIdentifier() == String::fromUTF8(installation.identifier)) {
                respond(B_NAME_IN_USE);
                return;
            }
        }
    }
    if (!validExtensionIdentity(options.uniqueIdentifier) || token.isEmpty()
        || options.expectedFingerprint.size() != 64 || options.permissions.size() + options.origins.size() > maximumExtensionGrants) {
        respond(B_BAD_VALUE);
        return;
    }
    if (auto error = details.AddString("extension_identifier", options.uniqueIdentifier.c_str()); error != B_OK) {
        respond(error);
        return;
    }
    if (m_shutdown) {
        respond(B_CANCELED);
        return;
    }
    auto found = m_prepared.find(token);
    if (found == m_prepared.end()) {
        respond(B_NAME_NOT_FOUND);
        return;
    }
    Ref extension = found->value.copyRef();
    if (String::fromUTF8(options.expectedFingerprint) != extension->nativeResourceFingerprint()) {
        refuse("The installed package is not the one that was approved.");
        return;
    }
    if (!extension->nativeVerifiedCRXIdentifier().isEmpty() && extension->nativeVerifiedCRXIdentifier() != uniqueIdentifier) {
        refuse("The package is signed for a different extension identifier.");
        return;
    }
    if (m_loaded.contains(uniqueIdentifier)
        || std::ranges::any_of(m_controller->extensionContexts(), [&](auto& context) { return context->uniqueIdentifier() == uniqueIdentifier; })) {
        respond(B_NAME_IN_USE);
        return;
    }
    bool restore = false;
    WebExtensionLoadPurposeHaiku purpose;
    switch (options.purpose) {
    case BWebKitExtensionLoadOptions::Purpose::UserApproved:
        purpose = m_privateBrowsing ? WebExtensionLoadPurposeHaiku::PrivateBrowsing : WebExtensionLoadPurposeHaiku::UserInitiated;
        break;
    case BWebKitExtensionLoadOptions::Purpose::BrowserStartup:
        purpose = WebExtensionLoadPurposeHaiku::BrowserStartup;
        restore = true;
        break;
    case BWebKitExtensionLoadOptions::Purpose::BrowserUpdate:
        purpose = WebExtensionLoadPurposeHaiku::BrowserUpdate;
        restore = true;
        break;
    default:
        respond(B_BAD_VALUE);
        return;
    }
    if (m_privateBrowsing && (!options.allowPrivateBrowsing || restore)) {
        refuse("This extension is not allowed in private windows.");
        return;
    }
    // Origins granted on this load besides the caller's: the package's own
    // requested hosts when a restored installation lost its saved grants.
    std::vector<std::string> recoveredOrigins;
    const char* recovery = nullptr;
    // Grants installed on the context instead of the saved ones.
    bool replacesGrants = !restore;
    String statePath, backupPath;
    if (m_controller->configuration().storageIsPersistent()) {
        auto directory = FileSystem::pathByAppendingComponent(m_controller->configuration().storageDirectory(), uniqueIdentifier);
        statePath = FileSystem::pathByAppendingComponent(directory, WebExtensionContext::stateFileName());
        backupPath = FileSystem::pathByAppendingComponent(directory, "State.backup.json"_s);
    }
    if (restore) {
        // A restored installation may name required manifest permissions that its original
        // consent already covered but an older engine could not provide. Origins never change here.
        if (!options.origins.empty() || !m_controller->configuration().storageIsPersistent()) {
            respond(B_BAD_VALUE);
            return;
        }
        for (auto& input : options.permissions) {
            if (!extension->requestedPermissions().contains(String::fromUTF8(input))) {
                refuse("The installation names a permission its package does not request.");
                return;
            }
        }
        auto fingerprint = extension->nativeResourceFingerprint();
        auto belongsToPackage = [&](const JSON::Object& state) {
            return state.getDouble("ResourceFingerprintFormat"_s) == 1 && state.getString("LastSeenResourceFingerprint"_s) == fingerprint;
        };
        auto state = readWebExtensionStateHaiku(statePath);
        if (!state || !belongsToPackage(state->get())) {
            // Saved grants for another version of the package do not carry over to this one.
            if (state && !(*state)->getString("LastSeenResourceFingerprint"_s).isEmpty()) {
                refuse("Its saved permissions belong to a different version of the package.");
                return;
            }
            // The saved state is missing or unreadable. A machine reset before the file reached
            // the disk can leave it holding other files' old blocks (a uBlock Origin state on the
            // X399 held 702 bytes of 1Password data). The host's catalog approved exactly this
            // package (the fingerprint checks above), so start it again: from the copy taken at
            // its last good start if there is one, else with the access its installation granted.
            // The damaged file is kept next to it for inspection.
            if (FileSystem::fileExists(statePath))
                FileSystem::moveFile(statePath, FileSystem::pathByAppendingComponent(FileSystem::parentPath(statePath), "State.damaged.json"_s));
            auto backup = readWebExtensionStateHaiku(backupPath);
            if (backup && belongsToPackage(backup->get()) && writeWebExtensionStateHaiku(statePath, backup->get()))
                recovery = "Its saved settings were damaged and have been restored from the copy taken when it last started.";
            else {
                recovery = "Its saved settings were damaged. It was started again with the access approved when it was installed; access granted later has to be granted again.";
                replacesGrants = true;
                // The same origins the installation consent listed and granted (see prepare()).
                auto patterns = copyToVector(extension->allRequestedMatchPatterns());
                for (auto& pattern : patterns) {
                    if (pattern->scheme() == "file"_s && !options.allowFileURLs)
                        continue;
                    bool covered = std::ranges::any_of(patterns, [&](auto& other) {
                        return other.ptr() != pattern.ptr() && other->matchesPattern(pattern) && !pattern->matchesPattern(other);
                    });
                    if (!covered)
                        recoveredOrigins.emplace_back(pattern->string().utf8().legacyCStringPointer());
                }
            }
        }
    }

    WebExtensionContext::PermissionsMap permissions;
    WebExtensionContext::PermissionMatchPatternsMap origins;
    auto allowedPermissions = extension->requestedPermissions().unionWith(extension->optionalPermissions());
    auto allowedOrigins = extension->allRequestedMatchPatterns().unionWith(extension->optionalPermissionMatchPatterns());
    size_t length = 0;
    for (auto* values : { &options.permissions, &options.origins, &std::as_const(recoveredOrigins) }) {
        for (auto& input : *values) {
            if (input.size() > 16384 - length) {
                respond(B_BAD_VALUE);
                return;
            }
            length += input.size();
        }
    }
    for (auto& input : options.permissions) {
        auto name = String::fromUTF8(input);
        if (name.isEmpty() || name.contains('\0') || !allowedPermissions.contains(name)) {
            refuse("The installation names a permission its package does not request.");
            return;
        }
        permissions.set(WTF::move(name), WallTime::infinity());
    }
    auto grantedOrigins = options.origins;
    grantedOrigins.insert(grantedOrigins.end(), recoveredOrigins.begin(), recoveredOrigins.end());
    if (options.permissions.size() + grantedOrigins.size() > maximumExtensionGrants) {
        respond(B_BAD_VALUE);
        return;
    }
    for (auto& input : grantedOrigins) {
        auto text = String::fromUTF8(input);
        RefPtr pattern = text.contains('\0') ? nullptr : WebExtensionMatchPattern::getOrCreate(text);
        if (!pattern || !pattern->isSupported() || (pattern->scheme() == "file"_s && !options.allowFileURLs)
            || !std::ranges::any_of(allowedOrigins, [&](auto& allowed) {
                return allowed->matchesPattern(*pattern, { WebExtensionMatchPattern::Options::IgnorePaths, WebExtensionMatchPattern::Options::AllowFileScheme });
            })) {
            respond(B_NOT_ALLOWED);
            return;
        }
        // Host grants apply to the complete origin, independent of URL paths.
        if (!pattern->matchesAllURLs())
            pattern = WebExtensionMatchPattern::getOrCreate(pattern->scheme(), pattern->host(), "/*"_s);
        origins.set(pattern.releaseNonNull(), WallTime::infinity());
    }
    Ref context = WebExtensionContext::create(extension.copyRef());
    context->setUniqueIdentifier(String { uniqueIdentifier });
    // Keep the origin this installation's website data was written under, so
    // IndexedDB and local storage stay with the extension across restarts.
    m_controller->restoreNativeBaseURL(context.get());
    context->setNativeLoadPurpose(purpose);
    if (!context->setNativeInstallationMetadata(String::fromUTF8(installation.identifier), installation.order)) {
        respond(B_BAD_VALUE);
        return;
    }
    context->setHasAccessToFileURLs(options.allowFileURLs);
    context->setHasAccessToPrivateData(options.allowPrivateBrowsing);
    if (replacesGrants) {
        // Replace both sets even when empty, preventing saved grants from being
        // merged into a fresh host approval by restoreNativePermissionState().
        context->setGrantedPermissions(WTF::move(permissions));
        context->setGrantedPermissionMatchPatterns(WTF::move(origins));
    }
    SUMMIT_EXTENSION_TIMING("load %s starting", options.uniqueIdentifier.c_str());
    auto loaded = m_controller->load(context.get());
    SUMMIT_EXTENSION_TIMING("load %s %s", options.uniqueIdentifier.c_str(), loaded && *loaded ? "done" : "failed");
    if (!loaded || !*loaded) {
        respond(B_ERROR, loaded ? nullptr : loaded.error());
        return;
    }
    if (restore && !permissions.isEmpty()) {
        WebExtensionContext::PermissionsSet upgraded;
        for (auto& name : permissions.keys()) {
            if (!context->grantedPermissions().contains(name) && !context->deniedPermissions().contains(name))
                upgraded.add(name);
        }
        if (!upgraded.isEmpty())
            context->grantPermissions(WTF::move(upgraded));
    }
    m_loaded.add(uniqueIdentifier, context.copyRef());
    context->setNativeActionChangeHandler(createSharedTask<void(WebExtensionAction&)>([weakThis = weak_from_this()](WebExtensionAction&) {
        if (auto registry = weakThis.lock())
            registry->actionsDidChange();
    }));
    auto error = details.AddString("fingerprint", extension->nativeResourceFingerprint().utf8().legacyCStringPointer());
    if (error == B_OK)
        error = details.AddString("base_url", context->baseURL().string().utf8().legacyCStringPointer());
    if (error == B_OK)
        error = details.AddString("installation_identifier", installation.identifier.c_str());
    if (error == B_OK)
        error = details.AddUInt64("installation_order", installation.order);
    // chrome_url_overrides.newtab (Momentum, Tabliss, ...): the host opens this page for new tabs.
    if (auto newTab = context->overrideNewTabPageURL(); error == B_OK && newTab.isValid())
        error = details.AddString("new_tab_url", newTab.string().utf8().legacyCStringPointer());
    if (recovery && error == B_OK)
        error = details.AddString("recovered", recovery);
    auto delivered = error == B_OK ? respond(B_OK) : error;
    if (delivered == B_OK) {
        m_prepared.remove(token);
        actionsDidChange();
        if (!statePath.isEmpty()) {
            // Keep a copy of the state this start used, for recovery above. The copy changes
            // only when the state does, so it is usually long on the disk when a reset comes.
            m_queue->dispatch([statePath = statePath.isolatedCopy(), backupPath = backupPath.isolatedCopy(),
                fingerprint = extension->nativeResourceFingerprint().isolatedCopy()] {
                auto state = readWebExtensionStateHaiku(statePath);
                if (!state || (*state)->getDouble("ResourceFingerprintFormat"_s) != 1
                    || (*state)->getString("LastSeenResourceFingerprint"_s) != fingerprint)
                    return;
                auto json = (*state)->toJSONString();
                if (auto backup = readWebExtensionStateHaiku(backupPath); backup && (*backup)->toJSONString() == json)
                    return;
                if (auto written = writeWebExtensionStateHaiku(backupPath, state->get()); !written)
                    std::fprintf(stderr, "Summit extension state: cannot keep a copy (%u)\n", static_cast<unsigned>(written.error()));
            });
        }
        return;
    }
    // If the host cannot receive the activation receipt, restore the prepared
    // token and remove the new runtime rather than losing track of it.
    auto unloaded = m_controller->unload(context.get());
    if (unloaded && *unloaded)
        m_loaded.remove(uniqueIdentifier);
    actionsDidChange();
    respond(delivered);
}

RefPtr<API::PageConfiguration> ExtensionPackageRegistryHaiku::viewConfiguration(const String& extensionIdentifier) const
{
    ASSERT(RunLoop::isMain());
    auto found = m_loaded.find(extensionIdentifier);
    if (m_shutdown || found == m_loaded.end() || !found->value->isLoaded())
        return nullptr;
    return found->value->webViewConfiguration(WebExtensionContext::WebViewPurpose::Tab);
}

void ExtensionPackageRegistryHaiku::unload(const String& extensionIdentifier, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    BMessage details;
    details.AddString("extension_identifier", extensionIdentifier.utf8().legacyCStringPointer());
    auto found = extensionIdentifier.isEmpty() ? m_loaded.end() : m_loaded.find(extensionIdentifier);
    if (found == m_loaded.end()) {
        reply(target, B_WEBKIT_EXTENSION_UNLOADED, identifier, B_NAME_NOT_FOUND, &details);
        return;
    }
    Ref context = found->value.copyRef();
    auto result = m_controller->unload(context.get());
    if (result && *result) {
        context->setNativeActionChangeHandler(nullptr);
        m_loaded.remove(extensionIdentifier);
        reply(target, B_WEBKIT_EXTENSION_UNLOADED, identifier, B_OK, &details);
        actionsDidChange();
        return;
    }
    if (!result && result.error())
        details.AddString("description", result.error()->localizedDescription().utf8().legacyCStringPointer());
    reply(target, B_WEBKIT_EXTENSION_UNLOADED, identifier, B_ERROR, &details);
}

status_t ExtensionPackageRegistryHaiku::setActionListener(const BMessenger& listener)
{
    ASSERT(RunLoop::isMain());
    if (m_shutdown)
        return B_CANCELED;
    if (listener.IsValid() && !listener.IsTargetLocal())
        return B_BAD_VALUE;
    m_actionListener = listener;
    actionsDidChange();
    return B_OK;
}

void ExtensionPackageRegistryHaiku::actionsDidChange()
{
    ASSERT(RunLoop::isMain());
    if (!m_shutdown && m_actionListener.IsValid()) {
        BMessage changed(B_WEBKIT_EXTENSION_ACTIONS_CHANGED);
        m_actionListener.SendMessage(&changed, static_cast<BHandler*>(nullptr), 0);
    }
}

void ExtensionPackageRegistryHaiku::actions(const BMessenger& owner, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    if (m_shutdown) {
        reply(target, B_WEBKIT_EXTENSION_ACTIONS, identifier, B_CANCELED);
        return;
    }
    auto windows = m_controller->nativeBrowserTabs()->windows();
    auto window = std::ranges::find_if(windows, [&](auto& entry) { return entry.messenger == owner; });
    if (!owner.IsValid() || window == windows.end() || !window->activePage) {
        reply(target, B_WEBKIT_EXTENSION_ACTIONS, identifier, B_NAME_NOT_FOUND);
        return;
    }
    BMessage details;
    status_t error = B_OK;
    Vector<String> identities;
    for (auto& entry : m_loaded)
        identities.append(entry.key);
    std::sort(identities.begin(), identities.end(), WTF::codePointCompareLessThan);
    for (auto& identity : identities) {
        RefPtr context = m_loaded.get(identity);
        if (!context || !context->isLoaded())
            continue;
        auto& extension = context->extension();
        if (!(extension.hasAction() || extension.hasBrowserAction() || extension.hasPageAction()))
            continue;
        RefPtr tab = context->getOrCreateNativeTab(*window->activePage);
        if (!tab || !tab->isValid() || !tab->extensionHasAccess())
            continue;
        Ref action = context->getOrCreateAction(tab.get());
        BMessage item;
        auto add = [&](const char* key, const String& value) {
            if (error == B_OK)
                error = item.AddString(key, value.utf8().legacyCStringPointer());
        };
        add("extension_identifier", identity);
        add("name", extension.displayName().left(512));
        add("title", action->label().left(512));
        add("badge", action->badgeText().left(64));
        if (error == B_OK)
            error = item.AddUInt32("badge_background_rgba", action->badgeBackgroundColor());
        if (error == B_OK)
            error = item.AddUInt32("badge_text_rgba", action->badgeTextColor());
        if (error == B_OK)
            error = item.AddUInt64("load_identifier", context->privilegedIdentifier().toUInt64());
        if (error == B_OK)
            error = item.AddUInt64("page_identifier", window->activePage->toUInt64());
        if (error == B_OK)
            error = item.AddBool("enabled", action->isEnabled());
        if (error == B_OK)
            error = item.AddBool("has_popup", action->presentsPopup());
        // icon_bgra is the 16 pixel icon; icon_bgra_32 the same icon for a
        // screen drawn at twice the density, from the extension's larger art.
        for (auto [dimension, key] : { std::pair { 16, "icon_bgra" }, std::pair { 32, "icon_bgra_32" } }) {
            RefPtr icon = action->icon({ static_cast<float>(dimension), static_cast<float>(dimension) });
            if (!icon)
                continue;
            RefPtr buffer = WebCore::ImageBuffer::create({ static_cast<float>(dimension), static_cast<float>(dimension) }, WebCore::RenderingMode::Unaccelerated,
                WebCore::RenderingPurpose::DOM, 1, WebCore::ColorSpace::SRGB(), WebCore::PixelFormat::BGRA8);
            if (!buffer)
                continue;
            icon->paint(buffer->context(), { 0, 0, static_cast<float>(dimension), static_cast<float>(dimension) });
            auto pixels = buffer->getPixelBuffer({ WebCore::AlphaPremultiplication::Unpremultiplied, WebCore::PixelFormat::BGRA8, WebCore::ColorSpace::SRGB() }, { 0, 0, dimension, dimension });
            if (error == B_OK && pixels && pixels->bytes().size() == static_cast<size_t>(dimension * dimension * 4))
                error = item.AddData(key, B_RAW_TYPE, pixels->bytes().data(), pixels->bytes().size());
        }
        if (error == B_OK)
            error = details.AddMessage("action", &item);
        if (error != B_OK)
            break;
    }
    reply(target, B_WEBKIT_EXTENSION_ACTIONS, identifier, error, error == B_OK ? &details : nullptr);
}

void ExtensionPackageRegistryHaiku::activateAction(const String& identity, const BMessenger& owner, uint64_t loadIdentifier,
    uint64_t pageIdentifier, const BMessenger& target, uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    BMessage details;
    details.AddString("extension_identifier", identity.utf8().legacyCStringPointer());
    auto respond = [&](status_t error) { reply(target, B_WEBKIT_EXTENSION_ACTION_ACTIVATED, identifier, error, &details); };
    RefPtr context = m_loaded.get(identity);
    if (m_shutdown || !context || !context->isLoaded() || context->privilegedIdentifier().toUInt64() != loadIdentifier) {
        respond(B_CANCELED);
        return;
    }
    auto registry = m_controller->nativeBrowserTabs();
    auto windows = registry->windows();
    auto window = std::ranges::find_if(windows, [&](auto& entry) { return entry.messenger == owner; });
    if (!owner.IsValid() || window == windows.end() || !window->activePage || window->activePage->toUInt64() != pageIdentifier
        || registry->focusedWindow() != window->identifier) {
        respond(B_CANCELED);
        return;
    }
    RefPtr tab = context->getOrCreateNativeTab(*window->activePage);
    auto& extension = context->extension();
    if (!tab || !tab->isValid() || !tab->extensionHasAccess()
        || !(extension.hasAction() || extension.hasBrowserAction() || extension.hasPageAction())
        || !context->getAction(tab.get())->isEnabled()) {
        respond(B_NOT_ALLOWED);
        return;
    }
    SUMMIT_EXTENSION_TIMING("action %s activated by the browser", identity.utf8().legacyCStringPointer());
    context->performAction(tab.get(), WebExtensionContext::UserTriggered::Yes);
    respond(B_OK);
}

void ExtensionPackageRegistryHaiku::setPrivateAccess(const String& identity, bool allowed)
{
    ASSERT(RunLoop::isMain());
    RefPtr context = m_loaded.get(identity);
    if (m_shutdown || !context || !context->isLoaded() || context->hasAccessToPrivateData() == allowed)
        return;
    context->setHasAccessToPrivateData(allowed);
    // Private windows show or drop the extension's action.
    actionsDidChange();
}

void ExtensionPackageRegistryHaiku::preloadAction(const String& identity, const BMessenger& owner, uint64_t loadIdentifier, uint64_t pageIdentifier)
{
    ASSERT(RunLoop::isMain());
    RefPtr context = m_loaded.get(identity);
    if (m_shutdown || !context || !context->isLoaded() || context->privilegedIdentifier().toUInt64() != loadIdentifier)
        return;
    auto registry = m_controller->nativeBrowserTabs();
    auto windows = registry->windows();
    auto window = std::ranges::find_if(windows, [&](auto& entry) { return entry.messenger == owner; });
    if (!owner.IsValid() || window == windows.end() || !window->activePage || window->activePage->toUInt64() != pageIdentifier
        || registry->focusedWindow() != window->identifier)
        return;
    RefPtr tab = context->getOrCreateNativeTab(*window->activePage);
    if (!tab || !tab->isValid())
        return;
    ExtensionActionPopupHaiku::preload(*context, *tab);
}

} // namespace WebKit
#endif
