/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "Logging.h"
#include "APIContentRuleList.h"
#include "ExtensionActionPopupHaiku.h"
#include "WebExtensionStateHaiku.h"
#include "WebExtensionPermissionState.h"
#include "WebExtensionBackgroundListenersHaiku.h"
#include "WebExtensionWebsiteData.h"
#include "WebsiteDataStore.h"
#include "WebsiteDataFetchOption.h"
#include <WebCore/SecurityOriginData.h>
#include <wtf/FileSystem.h>
#include <wtf/JSONValues.h>
#include <wtf/text/MakeString.h>

namespace WebKit {

void WebExtensionContext::determineInstallReasonDuringLoad()
{
    ASSERT(isLoaded());
    Ref extension = *m_extension;
    auto purpose = std::exchange(m_nativeLoadPurpose, WebExtensionLoadPurposeHaiku::UserInitiated);
    RefPtr controller = extensionController();
    if (!controller)
        return;
    if (controller->configuration().defaultWebsiteDataStore().sessionID().isEphemeral())
        purpose = WebExtensionLoadPurposeHaiku::PrivateBrowsing;
    auto decision = updateWebExtensionInstallStateHaiku(*m_state, extension->version(), extension->nativeResourceFingerprint(), purpose);
    m_previousVersion = WTF::move(decision.previousVersion);
    m_shouldFireStartupEvent = decision.fireStartup;
    switch (decision.reason) {
    case WebExtensionInstallReasonHaiku::None: m_installReason = InstallReason::None; break;
    case WebExtensionInstallReasonHaiku::Install: m_installReason = InstallReason::ExtensionInstall; break;
    case WebExtensionInstallReasonHaiku::Update: m_installReason = InstallReason::ExtensionUpdate; break;
    case WebExtensionInstallReasonHaiku::BrowserUpdate: m_installReason = InstallReason::BrowserUpdate; break;
    }
    if (decision.clearCachedState) {
        clearBackgroundListenersHaiku(*m_state);
        m_backgroundContentEventListeners.clear();
        clearDeclarativeNetRequestRulesetState();
        clearRegisteredContentScripts();
    }
}

void WebExtensionContext::removeStaleExtensionWebsiteData()
{
    if (!storageIsPersistent())
        return;

    RefPtr controller = extensionController();
    if (!controller || !controller->markDidRemoveStaleExtensionWebsiteData())
        return;

    auto sentinelPath = FileSystem::pathByAppendingComponent(controller->configuration().storageDirectory(), "StaleExtensionOriginsCleared"_s);
    if (FileSystem::fileExists(sentinelPath))
        return;

    Ref websiteDataStore = controller->configuration().defaultWebsiteDataStore();
    auto dataTypes = webExtensionWebsiteDataTypes();
    websiteDataStore->fetchData(dataTypes, { WebsiteDataFetchOption::IncludeAllOrigins }, [protectedThis = Ref { *this }, dataTypes, websiteDataStore, sentinelPath](Vector<WebsiteDataRecord> records) {
        RefPtr controller = protectedThis->extensionController();
        if (!controller)
            return;

        auto activeExtensionURLs = controller->activeExtensionURLs();

        auto staleRecords = staleWebExtensionWebsiteData(records, activeExtensionURLs);

        if (staleRecords.isEmpty()) {
            FileSystem::overwriteEntireFile(sentinelPath, { });
            return;
        }

        websiteDataStore->removeData(dataTypes, staleRecords, [sentinelPath] {
            FileSystem::overwriteEntireFile(sentinelPath, { });
        });
    });
}


String WebExtensionContext::declarativeNetRequestContentRuleListFilePath()
{
    if (!m_declarativeNetRequestContentRuleListFilePath.isEmpty())
        return m_declarativeNetRequestContentRuleListFilePath;

    auto directoryPath = storageIsPersistent() ? storageDirectory() : FileSystem::createTemporaryDirectory();
    if (directoryPath.isEmpty())
        return nullString();

    m_declarativeNetRequestContentRuleListFilePath = FileSystem::pathByAppendingComponent(directoryPath, "DeclarativeNetRequestContentRuleList.data"_s);

    return m_declarativeNetRequestContentRuleListFilePath;
}

static constexpr auto lastSeenBaseURLStateKey = "LastSeenBaseURL"_s;
static constexpr auto lastSeenDisplayNameStateKey = "LastSeenDisplayName"_s;
static constexpr auto declarativeNetRequestRulesetStateKey = "DeclarativeNetRequestRulesetState"_s;
static constexpr auto storageAccessLevelsKey = "StorageAccessLevels"_s;
static constexpr auto sessionStorageAllowedInContentScriptsKey = "SessionStorageAllowedInContentScripts"_s;

WebExtensionContext::WebExtensionContext(Ref<WebExtension>&& extension)
    : WebExtensionContext()
{
    m_extension = WTF::move(extension);
    m_state = JSON::Object::create();
    m_baseURL = URL { makeString("webkit-extension://"_s, uniqueIdentifier(), '/') };
}

void WebExtensionContext::recordError(Ref<API::Error> error)
{
    RELEASE_LOG_ERROR(Extensions, "Error recorded: %" PUBLIC_LOG_STRING, error->localizedDescription().utf8().legacyCStringPointer());
    if (m_errors.containsIf([&](auto& existing) { return existing->localizedDescription() == error->localizedDescription(); }))
        return;
    m_errors.append(WTF::move(error));
}

void WebExtensionContext::clearError(Error error)
{
    auto code = toAPIError(error);
    m_errors.removeAllMatching([&](auto& existing) { return existing->errorCode() == code; });
}

RefPtr<JSON::Object> WebExtensionContext::readStateFromPath(const String& path)
{
    auto result = readWebExtensionStateHaiku(path);
    if (result)
        return result.value().ptr();
    RELEASE_LOG_ERROR(Extensions, "Unable to read extension JSON state: %u", static_cast<unsigned>(result.error()));
    return JSON::Object::create();
}

bool WebExtensionContext::readLastBaseURLFromState(const String& path, URL& outLastBaseURL)
{
    outLastBaseURL = URL { readStateFromPath(path)->getString(lastSeenBaseURLStateKey) };
    return outLastBaseURL.isValid();
}

bool WebExtensionContext::readDisplayNameFromState(const String& path, String& outDisplayName)
{
    outDisplayName = readStateFromPath(path)->getString(lastSeenDisplayNameStateKey);
    return !outDisplayName.isEmpty();
}

RefPtr<const JSON::Object> WebExtensionContext::currentState() const
{
    return m_state;
}

RefPtr<JSON::Object> WebExtensionContext::readStateFromStorage()
{
    resetCommands();
    if (storageIsPersistent()) {
        auto result = readWebExtensionStateHaiku(stateFilePath());
        if (!result) {
            RELEASE_LOG_ERROR(Extensions, "Unable to read extension JSON state: %u", static_cast<unsigned>(result.error()));
            return nullptr;
        }
        m_state = result->ptr();
    } else if (!m_state)
        m_state = JSON::Object::create();
    return m_state;
}

void WebExtensionContext::writeStateToStorage() const
{
    if (!storageIsPersistent())
        return;
    RefPtr state = currentState();
    if (!state)
        return;

    if (m_nativePermissionStateInitialized) {
        auto prepared = stateWithWebExtensionPermissions(state.get(), nativePermissionState());
        if (!prepared) {
            RELEASE_LOG_ERROR(Extensions, "Unable to prepare native permission state");
            return;
        }
        state = prepared->ptr();
    }
    if (auto result = writeWebExtensionStateHaiku(stateFilePath(), *state); !result)
        RELEASE_LOG_ERROR(Extensions, "Unable to write extension JSON state: %u", static_cast<unsigned>(result.error()));
}

URL WebExtensionContext::updatePersistedMetadataDuringLoad()
{
    ASSERT(m_state);
    URL previousBaseURL { m_state->getString(lastSeenBaseURLStateKey) };
    m_state->setString(lastSeenBaseURLStateKey, m_baseURL.string());
    auto displayName = protect(m_extension)->displayName();
    if (!displayName.isNull())
        m_state->setString(lastSeenDisplayNameStateKey, displayName);
    return previousBaseURL;
}

void WebExtensionContext::loadDeclarativeNetRequestRulesetStateFromStorage()
{
    ASSERT(m_state);
    HashMap<String, bool> defaults;
    for (auto& ruleset : protect(extension())->declarativeNetRequestRulesets())
        defaults.set(ruleset.rulesetID, ruleset.enabled);
    auto savedState = m_state->getValue(declarativeNetRequestRulesetStateKey);
    m_enabledStaticRulesetIDs = enabledWebExtensionRulesetsHaiku(defaults, savedState.get());
}

void WebExtensionContext::clearDeclarativeNetRequestRulesetState()
{
    ASSERT(m_state);
    m_state->remove(declarativeNetRequestRulesetStateKey);
    m_enabledStaticRulesetIDs.clear();
}

void WebExtensionContext::loadStorageAccessLevelsFromStorage()
{
    ASSERT(m_state);
    RefPtr savedLevels = m_state->getObject(storageAccessLevelsKey);
    bool legacyAllowed = m_state->getBoolean(sessionStorageAllowedInContentScriptsKey).value_or(false);
    m_state->remove(sessionStorageAllowedInContentScriptsKey);
    if (savedLevels) {
        for (auto dataType : allWebExtensionDataTypes()) {
            auto value = savedLevels->getString(toAPIString(dataType));
            // Reject malformed saved values without passing them to the enum
            // converter, whose debug assertion assumes API-validated input.
            if (value != "TRUSTED_CONTEXTS"_s && value != "TRUSTED_AND_UNTRUSTED_CONTEXTS"_s)
                continue;
            if (auto accessLevel = toWebExtensionStorageAccessLevel(value))
                m_storageAccessLevels.set(dataType, *accessLevel);
        }
        return;
    }
    if (legacyAllowed) {
        m_storageAccessLevels.set(WebExtensionDataType::Session, WebExtensionStorageAccessLevel::TrustedAndUntrustedContexts);
        saveStorageAccessLevelsToStorage();
    }
}

void WebExtensionContext::saveStorageAccessLevelsToStorage()
{
    ASSERT(m_state);
    Ref levels = JSON::Object::create();
    for (auto [dataType, accessLevel] : m_storageAccessLevels)
        levels->setString(toAPIString(dataType), toAPIString(accessLevel));
    m_state->setObject(storageAccessLevelsKey, WTF::move(levels));
    writeStateToStorage();
}

void WebExtensionContext::moveLocalStorageIfNeeded(const URL& previousBaseURL, CompletionHandler<void()>&& completionHandler)
{
    if (!previousBaseURL.isValid() || previousBaseURL == baseURL()) {
        completionHandler();
        return;
    }
    extensionController()->configuration().defaultWebsiteDataStore().renameOriginInWebsiteData(
        WebCore::SecurityOriginData::fromURL(previousBaseURL), WebCore::SecurityOriginData::fromURL(baseURL()),
        { WebsiteDataType::IndexedDBDatabases, WebsiteDataType::LocalStorage }, WTF::move(completionHandler));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
