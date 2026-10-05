/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "ExtensionPermissionPromptHaiku.h"
#include "WebExtensionController.h"
#include "WebExtensionControllerConfiguration.h"
#include "WebExtensionPermissionState.h"
#include "WebExtensionStateHaiku.h"
#include "WebExtensionUtilities.h"
#include "WebsiteDataStore.h"
#include <algorithm>
#include <wtf/SetForScope.h>

namespace WebKit {

using Patterns = WebExtensionContext::MatchPatternSet;
using Permissions = WebExtensionContext::PermissionsSet;

static RefPtr<WebExtensionMatchPattern> permissionPattern(const String& origin)
{
    if (origin.contains('\0'))
        return nullptr;
    auto pattern = WebExtensionMatchPattern::getOrCreate(origin);
    if (!pattern || !pattern->isSupported())
        return nullptr;
    if (pattern->matchesAllURLs())
        return pattern;
    // The permissions API always ignores paths, including in saved grants.
    return WebExtensionMatchPattern::getOrCreate(pattern->scheme(), pattern->host(), "/*"_s);
}

static bool covers(const WebExtensionMatchPattern& covering, const WebExtensionMatchPattern& requested)
{
    return covering.matchesPattern(requested, { WebExtensionMatchPattern::Options::IgnorePaths, WebExtensionMatchPattern::Options::AllowFileScheme });
}

static bool covers(const Patterns& patterns, const WebExtensionMatchPattern& requested)
{
    return std::ranges::any_of(patterns, [&](auto& pattern) { return covers(pattern.get(), requested); });
}

static bool overlaps(const WebExtensionMatchPattern& first, const WebExtensionMatchPattern& second)
{
    // Bidirectional matching compares each component in both directions, so
    // intersecting scheme and subdomain wildcards need not cover one another.
    return first.matchesPattern(second, { WebExtensionMatchPattern::Options::IgnorePaths,
        WebExtensionMatchPattern::Options::MatchBidirectionally, WebExtensionMatchPattern::Options::AllowFileScheme });
}

static bool containsGrantedPermissions(WebExtensionContext& context, const Permissions& names, const Patterns& patterns)
{
    const auto& grantedNames = context.grantedPermissions();
    const auto& grantedPatterns = context.grantedPermissionMatchPatterns();
    if (!std::ranges::all_of(names, [&](auto& name) { return grantedNames.contains(name); }))
        return false;
    OptionSet options { WebExtensionMatchPattern::Options::IgnorePaths };
    if (context.hasAccessToFileURLs())
        options.add(WebExtensionMatchPattern::Options::AllowFileScheme);
    return std::ranges::all_of(patterns, [&](auto& pattern) {
        if (pattern->scheme() == "file"_s && !context.hasAccessToFileURLs())
            return false;
        return std::ranges::any_of(grantedPatterns, [&](auto& entry) { return entry.key->matchesPattern(pattern.get(), options); });
    });
}

enum class PermissionOperation { Contains, Request, Remove };

static std::expected<Patterns, WebExtensionError> validatePermissions(WebExtensionContext& context, const Permissions& permissions, const HashSet<String>& origins, PermissionOperation operation, const String& api)
{
    auto& extension = context.extension();
    auto allowedNames = extension.requestedPermissions().unionWith(extension.optionalPermissions());
    auto requiredPatterns = extension.allRequestedMatchPatterns();
    auto allowedPatterns = requiredPatterns.unionWith(extension.optionalPermissionMatchPatterns());
    for (auto& permission : permissions) {
        if (!WebExtension::supportedPermissions().contains(permission))
            return toWebExtensionError(api, "permissions"_s, "unsupported permission"_s);
        if (operation != PermissionOperation::Contains && !allowedNames.contains(permission))
            return toWebExtensionError(api, "permissions"_s, "permission was not declared in the manifest"_s);
        if (operation == PermissionOperation::Remove && extension.requestedPermissions().contains(permission))
            return toWebExtensionError(api, "permissions"_s, "required permissions cannot be removed"_s);
    }
    Patterns patterns;
    for (auto& origin : origins) {
        auto pattern = permissionPattern(origin);
        if (!pattern)
            return toWebExtensionError(api, "origins"_s, "unsupported or invalid match pattern"_s);
        if (operation == PermissionOperation::Request && pattern->scheme() == "file"_s && !context.hasAccessToFileURLs())
            return toWebExtensionError(api, "origins"_s, "file access must first be enabled by the browser"_s);
        if (operation != PermissionOperation::Contains && !covers(allowedPatterns, *pattern))
            return toWebExtensionError(api, "origins"_s, "origin was not declared in the manifest"_s);
        if (operation == PermissionOperation::Remove && std::ranges::any_of(requiredPatterns, [&](auto& required) { return overlaps(required.get(), *pattern); }))
            return toWebExtensionError(api, "origins"_s, "required host permissions cannot be removed"_s);
        patterns.add(pattern.releaseNonNull());
    }
    return patterns;
}

WebExtensionPermissionState WebExtensionContext::nativePermissionState() const
{
    WebExtensionPermissionState result { m_grantedPermissions, { } };
    for (auto& entry : m_grantedPermissionMatchPatterns)
        result.origins.set(entry.key->string(), entry.value);
    return result;
}

std::expected<void, String> WebExtensionContext::restoreNativePermissionState()
{
    ASSERT(!isLoaded());
    auto now = WallTime::now();
    auto saved = readWebExtensionPermissionState(m_state.get(), now);
    if (!saved)
        return makeUnexpected(saved.error());

    // A context keeps its current host/API state across unload/reload. A fresh
    // context restores disk grants, respecting explicit host replacements and
    // revocations made before load. Nothing is granted merely by declaration.
    auto names = m_nativePermissionStateInitialized || m_nativeGrantedPermissionsWereSet
        ? m_grantedPermissions : saved->permissions;
    if (!m_nativePermissionStateInitialized && !m_nativeGrantedPermissionsWereSet) {
        for (auto& removed : m_nativeRemovedPermissionsBeforeLoad)
            names.remove(removed);
        for (auto& entry : m_grantedPermissions)
            names.set(entry.key, entry.value);
    }
    PermissionMatchPatternsMap patterns;
    if (m_nativePermissionStateInitialized || m_nativeGrantedPatternsWereSet)
        patterns = m_grantedPermissionMatchPatterns;
    else {
        for (auto& entry : saved->origins) {
            auto pattern = permissionPattern(entry.key);
            if (!pattern)
                continue;
            bool removed = std::ranges::any_of(m_nativeRemovedPatternsBeforeLoad, [&](auto& removal) {
                return removal.second == EqualityOnly::Yes ? removal.first.get() == *pattern : covers(removal.first.get(), *pattern);
            });
            if (!removed)
                patterns.set(pattern.releaseNonNull(), entry.value);
        }
        for (auto& entry : m_grantedPermissionMatchPatterns)
            patterns.set(entry.key, entry.value);
    }

    auto& extension = this->extension();
    auto allowedNames = extension.requestedPermissions().unionWith(extension.optionalPermissions());
    auto allowedPatterns = extension.allRequestedMatchPatterns().unionWith(extension.optionalPermissionMatchPatterns());
    names.removeIf([&](auto& entry) {
        auto denied = m_deniedPermissions.find(entry.key);
        return entry.value.isNaN() || entry.value <= now || !allowedNames.contains(entry.key)
            || !WebExtension::supportedPermissions().contains(entry.key)
            || (denied != m_deniedPermissions.end() && denied->value > now);
    });
    patterns.removeIf([&](auto& entry) {
        if (entry.value.isNaN() || entry.value <= now || !entry.key->isSupported() || !covers(allowedPatterns, entry.key.get()))
            return true;
        return std::ranges::any_of(m_deniedPermissionMatchPatterns, [&](auto& denied) {
            return denied.value > now && covers(denied.key.get(), entry.key.get());
        });
    });
    WebExtensionPermissionState cleaned { names, { } };
    for (auto& entry : patterns)
        cleaned.origins.set(entry.key->string(), entry.value);
    auto state = stateWithWebExtensionPermissions(m_state.get(), cleaned);
    if (!state)
        return makeUnexpected(state.error());
    // Restore before attaching the controller: no permission events or IPC
    // may run before the receiving extension contexts have been created.
    m_grantedPermissions = WTF::move(names);
    m_grantedPermissionMatchPatterns = WTF::move(patterns);
    m_nextGrantedPermissionsExpirationDate = WallTime::nan();
    m_nextGrantedPermissionMatchPatternsExpirationDate = WallTime::nan();
    clearCachedPermissionStates();
    m_state = WTF::move(*state);
    m_nativePermissionStateInitialized = true;
    m_nativeGrantedPermissionsWereSet = false;
    m_nativeGrantedPatternsWereSet = false;
    m_nativeRemovedPermissionsBeforeLoad.clear();
    m_nativeRemovedPatternsBeforeLoad.clear();
    return { };
}

std::expected<void, WebExtensionError> WebExtensionContext::commitNativePermissionState(PermissionsMap&& names, PermissionMatchPatternsMap&& patterns, const String& api)
{
    if (!isLoaded())
        return toWebExtensionError(api, nullString(), "extension is not loaded"_s);
    WebExtensionPermissionState values { names, { } };
    for (auto& entry : patterns)
        values.origins.set(entry.key->string(), entry.value);
    auto prepared = stateWithWebExtensionPermissions(m_state.get(), values);
    if (!prepared)
        return toWebExtensionError(api, nullString(), prepared.error());
    if (storageIsPersistent()) {
        if (auto written = writeWebExtensionStateHaiku(stateFilePath(), prepared->get()); !written)
            return toWebExtensionError(api, nullString(), "could not save permission grants"_s);
    }

    auto previousNames = m_grantedPermissions;
    auto previousPatterns = m_grantedPermissionMatchPatterns;
    m_state = WTF::move(*prepared);
    {
        SetForScope transaction(m_nativePermissionTransaction, true);
        setGrantedPermissions(WTF::move(names));
        setGrantedPermissionMatchPatterns(WTF::move(patterns));
    }
    // A finite grant can expire during the disk write. Compare the actual
    // installed state, after the common setters have pruned expired entries.
    Permissions addedNames, removedNames;
    Patterns addedPatterns, removedPatterns;
    for (auto& entry : m_grantedPermissions) {
        if (!previousNames.contains(entry.key))
            addedNames.add(entry.key);
    }
    for (auto& entry : previousNames) {
        if (!m_grantedPermissions.contains(entry.key))
            removedNames.add(entry.key);
    }
    for (auto& entry : m_grantedPermissionMatchPatterns) {
        if (!previousPatterns.contains(entry.key))
            addedPatterns.add(entry.key);
    }
    for (auto& entry : previousPatterns) {
        if (!m_grantedPermissionMatchPatterns.contains(entry.key))
            removedPatterns.add(entry.key);
    }
    // Publish one event containing both categories after the complete new
    // state is installed. Native observers and engine side effects still run.
    if (!removedNames.isEmpty() || !removedPatterns.isEmpty())
        firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnRemoved, removedNames, removedPatterns);
    if (!addedNames.isEmpty() || !addedPatterns.isEmpty())
        firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnAdded, addedNames, addedPatterns);
    return { };
}

void WebExtensionContext::permissionsGetAll(CompletionHandler<void(Vector<String>&&, Vector<String>&&)>&& completion)
{
    Vector<String> names, origins;
    for (auto& permission : currentPermissions())
        names.append(permission);
    for (auto& pattern : currentPermissionMatchPatterns()) {
        if (pattern->scheme() != "file"_s || hasAccessToFileURLs())
            origins.append(pattern->string());
    }
    completion(WTF::move(names), WTF::move(origins));
}

void WebExtensionContext::permissionsContains(HashSet<String> names, HashSet<String> origins, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&& completion)
{
    auto patterns = validatePermissions(*this, names, origins, PermissionOperation::Contains, "permissions.contains()"_s);
    if (!patterns) {
        completion(makeUnexpected(patterns.error()));
        return;
    }
    grantedPermissions();
    grantedPermissionMatchPatterns();
    if (!hasAccessToFileURLs() && std::ranges::any_of(*patterns, [](auto& pattern) { return pattern->scheme() == "file"_s; })) {
        completion(false);
        return;
    }
    completion(containsGrantedPermissions(*this, names, *patterns));
}

void WebExtensionContext::permissionsRequest(HashSet<String> names, HashSet<String> origins, bool userGesture, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&& completion)
{
    constexpr auto api = "permissions.request()"_s;
    if (!userGesture) {
        completion(toWebExtensionError(api, nullString(), "a user gesture is required"_s));
        return;
    }
    auto patterns = validatePermissions(*this, names, origins, PermissionOperation::Request, api);
    if (!patterns) {
        completion(makeUnexpected(patterns.error()));
        return;
    }
    grantedPermissions();
    grantedPermissionMatchPatterns();
    if (containsGrantedPermissions(*this, names, *patterns)) {
        completion(true);
        return;
    }
    RefPtr controller = extensionController();
    if (!controller) {
        completion(toWebExtensionError(api, nullString(), "extension has no browser host"_s));
        return;
    }
    ExtensionPermissionPromptHaiku::Details details;
    details.extensionIdentifier = uniqueIdentifier();
    details.extensionName = extension().displayName();
    details.privateBrowsing = !controller->configuration().defaultWebsiteDataStore().isPersistent();
    Permissions promptedNames;
    Patterns promptedPatterns;
    for (auto& name : names) {
        if (!m_grantedPermissions.contains(name)) {
            details.permissions.append(name);
            promptedNames.add(name);
        }
    }
    for (auto& pattern : *patterns) {
        if (!containsGrantedPermissions(*this, { }, { pattern })) {
            details.origins.append(pattern->string());
            promptedPatterns.add(pattern);
        }
    }
    auto loadIdentifier = privilegedIdentifier();
    auto revocationGeneration = m_nativePermissionRevocationGeneration;
    ++m_pendingPermissionRequests;
    controller->nativePermissionPrompts()->request(loadIdentifier.toUInt64(), WTF::move(details),
        [this, protectedThis = Ref { *this }, loadIdentifier, revocationGeneration, names = WTF::move(names), origins = WTF::move(origins), promptedNames = WTF::move(promptedNames), promptedPatterns = WTF::move(promptedPatterns), completion = WTF::move(completion)](std::expected<bool, status_t> consent) mutable {
            if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier) {
                completion(toWebExtensionError("permissions.request()"_s, nullString(), "extension was unloaded"_s));
                return;
            }
            ASSERT(m_pendingPermissionRequests);
            --m_pendingPermissionRequests;
            if (!consent) {
                auto reason = consent.error() == B_TIMED_OUT ? "permission prompt timed out"_s
                    : consent.error() == B_CANCELED ? "permission request was cancelled"_s
                    : consent.error() == B_BUSY ? "permission prompt queue is full"_s
                    : consent.error() == B_NO_INIT ? "browser has no permission prompt listener"_s
                    : "permission prompt could not complete"_s;
                completion(toWebExtensionError("permissions.request()"_s, nullString(), reason));
                return;
            }
            if (!*consent) {
                completion(false);
                return;
            }
            if (m_nativePermissionRevocationGeneration != revocationGeneration) {
                completion(toWebExtensionError("permissions.request()"_s, nullString(), "permissions changed while consent was pending"_s));
                return;
            }
            auto checked = validatePermissions(*this, names, origins, PermissionOperation::Request, "permissions.request()"_s);
            if (!checked) {
                completion(makeUnexpected(checked.error()));
                return;
            }
            auto updatedNames = grantedPermissions();
            auto updatedPatterns = grantedPermissionMatchPatterns();
            // Expiration can itself revoke a grant while taking the snapshots.
            if (m_nativePermissionRevocationGeneration != revocationGeneration) {
                completion(toWebExtensionError("permissions.request()"_s, nullString(), "permissions expired while consent was pending"_s));
                return;
            }
            // Existing temporary grants were not displayed in the prompt and
            // must not silently become permanent when other access is allowed.
            for (auto& name : promptedNames)
                updatedNames.set(name, WallTime::infinity());
            for (auto& pattern : promptedPatterns)
                updatedPatterns.set(pattern, WallTime::infinity());
            auto committed = commitNativePermissionState(WTF::move(updatedNames), WTF::move(updatedPatterns), "permissions.request()"_s);
            if (!committed)
                completion(makeUnexpected(committed.error()));
            else
                completion(containsGrantedPermissions(*this, names, *checked));
        });
}

void WebExtensionContext::permissionsRemove(HashSet<String> names, HashSet<String> origins, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&& completion)
{
    auto patterns = validatePermissions(*this, names, origins, PermissionOperation::Remove, "permissions.remove()"_s);
    if (!patterns) {
        completion(makeUnexpected(patterns.error()));
        return;
    }
    auto updatedNames = grantedPermissions();
    auto updatedPatterns = grantedPermissionMatchPatterns();
    for (auto& name : names)
        updatedNames.remove(name);
    updatedPatterns.removeIf([&](auto& entry) { return covers(*patterns, entry.key.get()); });
    auto committed = commitNativePermissionState(WTF::move(updatedNames), WTF::move(updatedPatterns), "permissions.remove()"_s);
    if (!committed) {
        completion(makeUnexpected(committed.error()));
        return;
    }
    // Removing an absent optional permission (including an empty set) succeeds.
    // A narrower request cannot claim to remove access retained by a broad grant.
    bool stillGranted = std::ranges::any_of(names, [&](auto& name) { return m_grantedPermissions.contains(name); })
        || std::ranges::any_of(*patterns, [&](auto& pattern) { return containsGrantedPermissions(*this, { }, { pattern }); });
    completion(!stillGranted);
}

} // namespace WebKit

#endif
