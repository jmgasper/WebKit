/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <array>
#include <expected>
#include <optional>
#include <wtf/JSONValues.h>

namespace WebKit {
enum class WebExtensionPrivacySettingHaiku : uint8_t { NetworkPrediction, HyperlinkAuditing };
enum class WebExtensionPrivacyScopeHaiku : uint8_t { Regular, RegularOnly, IncognitoPersistent, IncognitoSessionOnly };
using WebExtensionPrivacyValuesHaiku = std::array<std::optional<bool>, 2>;
struct WebExtensionPrivacyValueHaiku {
    bool value;
    bool incognitoSpecific;
};
struct WebExtensionPrivacySettingsHaiku {
    // Session overrides belong to the controller, so they survive disabling an extension
    // within the same private session and can never enter this persisted representation.
    std::array<WebExtensionPrivacyValuesHaiku, 3> values;
    std::optional<WebExtensionPrivacyValueHaiku> effective(WebExtensionPrivacySettingHaiku, bool incognito,
        const WebExtensionPrivacyValuesHaiku* sessionValues = nullptr) const;
    bool set(WebExtensionPrivacySettingHaiku, WebExtensionPrivacyScopeHaiku, std::optional<bool>);
};
std::optional<WebExtensionPrivacySettingHaiku> parseWebExtensionPrivacySettingHaiku(const String&);
std::optional<WebExtensionPrivacyScopeHaiku> parseWebExtensionPrivacyScopeHaiku(const String&);
String webExtensionPrivacySettingNameHaiku(WebExtensionPrivacySettingHaiku);
std::expected<WebExtensionPrivacySettingsHaiku, String> parseWebExtensionPrivacySettingsHaiku(const JSON::Object&, const String& installationIdentifier);
Ref<JSON::Object> serializeWebExtensionPrivacySettingsHaiku(const WebExtensionPrivacySettingsHaiku&, const String& installationIdentifier);
}
#endif
