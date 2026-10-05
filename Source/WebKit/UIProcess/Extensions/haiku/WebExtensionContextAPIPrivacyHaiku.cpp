/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionController.h"
#include "WebExtensionPermission.h"
#include "WebExtensionPermissionState.h"
#include "WebExtensionPrivacyControllerHaiku.h"
#include "WebExtensionStateHaiku.h"

namespace WebKit {
static String privacyAccessError(WebExtensionContext& context, const String& setting)
{
    if (!context.isLoaded() || !context.hasPermission(WebExtensionPermission::privacy()))
        return "The extension does not have the privacy permission."_s;
    if (!context.nativeInstallationOrder() || context.nativeInstallationIdentifier().isEmpty())
        return "The browser host has not supplied this extension's installation priority."_s;
    if (setting == "network.webRTCIPHandlingPolicy"_s)
        return "WebRTC IP handling policy is not available in this WebKit build."_s;
    if (!parseWebExtensionPrivacySettingHaiku(setting))
        return "Unknown privacy setting."_s;
    return { };
}
static std::expected<Ref<JSON::Object>, String> privacyDetails(const String& detailsJSON)
{
    if (detailsJSON.length() > 8192)
        return makeUnexpected("Privacy setting details exceed the size limit."_s);
    RefPtr value = JSON::Value::parseJSON(detailsJSON);
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return makeUnexpected("Privacy setting details must be an object."_s);
    return object.releaseNonNull();
}
std::expected<void, String> WebExtensionContext::restoreNativePrivacySettings()
{
    auto restored = parseWebExtensionPrivacySettingsHaiku(*m_state, nativeInstallationIdentifier());
    if (!restored)
        return makeUnexpected(restored.error());
    m_nativePrivacySettings = WTF::move(*restored);
    RELEASE_ASSERT(++m_nativePrivacyGeneration);
    return { };
}
void WebExtensionContext::privacyGet(const String& settingName, const String& detailsJSON, CompletionHandler<void(String, String)>&& completion)
{
    auto error = privacyAccessError(*this, settingName);
    if (!error.isEmpty()) {
        completion({ }, WTF::move(error));
        return;
    }
    auto details = privacyDetails(detailsJSON);
    if (!details) {
        completion({ }, details.error());
        return;
    }
    bool incognito = false;
    if (RefPtr value = (*details)->getValue("incognito"_s)) {
        auto flag = value->asBoolean();
        if (!flag.has_value()) {
            completion({ }, "The incognito option must be a boolean."_s);
            return;
        }
        incognito = *flag;
    }
    if (incognito && !hasAccessToPrivateData()) {
        completion({ }, "The extension is not allowed in private browsing."_s);
        return;
    }
    auto& privacy = extensionController()->nativePrivacy();
    privacy.refresh();
    if (!hasPermission(WebExtensionPermission::privacy())) {
        completion({ }, "The privacy permission is no longer granted."_s);
        return;
    }
    completion(privacy.details(*this, *parseWebExtensionPrivacySettingHaiku(settingName), incognito)->toJSONString(), { });
}
void WebExtensionContext::privacyUpdate(const String& settingName, const String& operation, const String& detailsJSON, CompletionHandler<void(bool, String)>&& completion)
{
    auto error = privacyAccessError(*this, settingName);
    if (!error.isEmpty()) {
        completion(false, WTF::move(error));
        return;
    }
    if (operation != "set"_s && operation != "clear"_s) {
        completion(false, "Unknown privacy setting operation."_s);
        return;
    }
    auto details = privacyDetails(detailsJSON);
    if (!details) {
        completion(false, details.error());
        return;
    }
    auto scope = WebExtensionPrivacyScopeHaiku::Regular;
    if (RefPtr value = (*details)->getValue("scope"_s)) {
        auto parsed = parseWebExtensionPrivacyScopeHaiku(value->asString());
        if (!parsed) {
            completion(false, "Invalid privacy setting scope."_s);
            return;
        }
        scope = *parsed;
    }
    std::optional<bool> value;
    if (operation == "set"_s) {
        RefPtr requested = (*details)->getValue("value"_s);
        value = requested ? requested->asBoolean() : std::nullopt;
        if (!value.has_value()) {
            completion(false, "This privacy setting requires a boolean value."_s);
            return;
        }
    }
    if ((scope == WebExtensionPrivacyScopeHaiku::IncognitoPersistent || scope == WebExtensionPrivacyScopeHaiku::IncognitoSessionOnly) && !hasAccessToPrivateData()) {
        completion(false, "The extension is not allowed in private browsing."_s);
        return;
    }
    auto& privacy = extensionController()->nativePrivacy();
    privacy.refresh();
    if (!hasPermission(WebExtensionPermission::privacy())) {
        completion(false, "The privacy permission is no longer granted."_s);
        return;
    }
    auto setting = *parseWebExtensionPrivacySettingHaiku(settingName);
    if (scope == WebExtensionPrivacyScopeHaiku::IncognitoSessionOnly) {
        if (operation == "set"_s && !privacy.hasPrivatePages()) {
            completion(false, "A private browsing session must be open before setting a session-only preference."_s);
            return;
        }
        privacy.setSessionValue(*this, setting, value);
        completion(true, { });
        return;
    }
    auto proposedSettings = m_nativePrivacySettings;
    RELEASE_ASSERT(proposedSettings.set(setting, scope, value));
    auto proposedState = stateWithWebExtensionPermissions(m_state.get(), nativePermissionState());
    if (!proposedState) {
        completion(false, proposedState.error());
        return;
    }
    (*proposedState)->setObject("PrivacySettings"_s, serializeWebExtensionPrivacySettingsHaiku(proposedSettings, nativeInstallationIdentifier()));
    if (storageIsPersistent()) {
        if (auto saved = writeWebExtensionStateHaiku(stateFilePath(), proposedState->get()); !saved) {
            completion(false, "The privacy setting could not be saved. The live setting was not changed."_s);
            return;
        }
    }
    m_state = proposedState->ptr();
    m_nativePrivacySettings = WTF::move(proposedSettings);
    privacy.refresh();
    completion(true, { });
}
}
#endif
