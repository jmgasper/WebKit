/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionPrivacySettingsHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
namespace WebKit {
static constexpr std::array settingNames { "network.networkPredictionEnabled"_s, "websites.hyperlinkAuditingEnabled"_s };
static constexpr std::array scopeNames { "regular"_s, "regular_only"_s, "incognito_persistent"_s, "incognito_session_only"_s };

std::optional<WebExtensionPrivacySettingHaiku> parseWebExtensionPrivacySettingHaiku(const String& name)
{
    for (size_t i = 0; i < settingNames.size(); ++i) {
        if (name == settingNames[i])
            return static_cast<WebExtensionPrivacySettingHaiku>(i);
    }
    return std::nullopt;
}
std::optional<WebExtensionPrivacyScopeHaiku> parseWebExtensionPrivacyScopeHaiku(const String& name)
{
    for (size_t i = 0; i < scopeNames.size(); ++i) {
        if (name == scopeNames[i])
            return static_cast<WebExtensionPrivacyScopeHaiku>(i);
    }
    return std::nullopt;
}
String webExtensionPrivacySettingNameHaiku(WebExtensionPrivacySettingHaiku setting)
{
    return settingNames[static_cast<size_t>(setting)];
}
std::optional<WebExtensionPrivacyValueHaiku> WebExtensionPrivacySettingsHaiku::effective(WebExtensionPrivacySettingHaiku setting,
    bool incognito, const WebExtensionPrivacyValuesHaiku* session) const
{
    size_t index = static_cast<size_t>(setting);
    if (incognito) {
        if (session && (*session)[index].has_value())
            return WebExtensionPrivacyValueHaiku { *(*session)[index], true };
        if (values[2][index].has_value())
            return WebExtensionPrivacyValueHaiku { *values[2][index], true };
    } else if (values[1][index].has_value())
        return WebExtensionPrivacyValueHaiku { *values[1][index], false };
    if (values[0][index].has_value())
        return WebExtensionPrivacyValueHaiku { *values[0][index], false };
    return std::nullopt;
}
bool WebExtensionPrivacySettingsHaiku::set(WebExtensionPrivacySettingHaiku setting, WebExtensionPrivacyScopeHaiku scope, std::optional<bool> value)
{
    size_t scopeIndex = static_cast<size_t>(scope);
    size_t settingIndex = static_cast<size_t>(setting);
    if (scopeIndex >= values.size() || settingIndex >= settingNames.size())
        return false;
    values[scopeIndex][settingIndex] = value;
    return true;
}
std::expected<WebExtensionPrivacySettingsHaiku, String> parseWebExtensionPrivacySettingsHaiku(const JSON::Object& state, const String& installationIdentifier)
{
    WebExtensionPrivacySettingsHaiku settings;
    RefPtr savedValue = state.getValue("PrivacySettings"_s);
    if (!savedValue || installationIdentifier.isEmpty())
        return settings;
    RefPtr saved = savedValue->asObject();
    if (!saved || saved->getString("installation_identifier"_s).isEmpty())
        return makeUnexpected("The saved privacy settings have an invalid installation identity."_s);
    if (saved->getString("installation_identifier"_s) != installationIdentifier)
        return settings;
    if (saved->getDouble("version"_s) != 1)
        return makeUnexpected("The saved privacy settings have an unsupported version."_s);
    RefPtr scopes = saved->getObject("values"_s);
    if (!scopes)
        return makeUnexpected("The saved privacy settings are invalid."_s);
    for (auto& scopeEntry : *scopes) {
        auto scope = parseWebExtensionPrivacyScopeHaiku(scopeEntry.key);
        RefPtr values = scopeEntry.value->asObject();
        if (!scope || *scope == WebExtensionPrivacyScopeHaiku::IncognitoSessionOnly || !values)
            return makeUnexpected("The saved privacy settings contain an invalid persistent scope."_s);
        for (auto& entry : *values) {
            auto setting = parseWebExtensionPrivacySettingHaiku(entry.key);
            auto value = entry.value->asBoolean();
            if (!setting || !value.has_value())
                return makeUnexpected("The saved privacy settings contain an invalid value."_s);
            settings.set(*setting, *scope, value);
        }
    }
    return settings;
}
Ref<JSON::Object> serializeWebExtensionPrivacySettingsHaiku(const WebExtensionPrivacySettingsHaiku& settings, const String& installationIdentifier)
{
    Ref result = JSON::Object::create();
    result->setInteger("version"_s, 1);
    result->setString("installation_identifier"_s, installationIdentifier);
    Ref scopes = JSON::Object::create();
    for (size_t scope = 0; scope < settings.values.size(); ++scope) {
        Ref values = JSON::Object::create();
        for (size_t setting = 0; setting < settingNames.size(); ++setting) {
            if (settings.values[scope][setting].has_value())
                values->setBoolean(settingNames[setting], *settings.values[scope][setting]);
        }
        if (values->size())
            scopes->setObject(scopeNames[scope], WTF::move(values));
    }
    result->setObject("values"_s, WTF::move(scopes));
    return result;
}
}
#endif
