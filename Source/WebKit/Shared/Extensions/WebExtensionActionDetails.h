/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionTabIdentifier.h"
#include "WebExtensionWindowIdentifier.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
enum class WebExtensionActionDetailsKind : uint8_t { Get, Title, BadgeText, Popup, BadgeBackgroundColor, BadgeTextColor };
struct WebExtensionActionDetails {
    std::optional<WebExtensionWindowIdentifier> window;
    std::optional<WebExtensionTabIdentifier> tab;
    // A null String resets inherited text; an empty String overrides it.
    String value;
    // RGBA arrays are packed as 0xRRGGBBAA. CSS strings remain in value for
    // asynchronous parsing in the UI process. Both absent means reset.
    std::optional<uint32_t> color;
};
std::expected<WebExtensionActionDetails, String> parseWebExtensionActionDetails(const JSON::Value*, WebExtensionActionDetailsKind);
std::expected<std::optional<WebExtensionTabIdentifier>, String> parseWebExtensionActionTab(const JSON::Value*);
inline constexpr size_t maximumWebExtensionActionIconJSONLength = 16 * 1024 * 1024;
inline constexpr size_t maximumWebExtensionActionIconRepresentations = 256;
bool isValidWebExtensionActionIconDimension(const String&);
std::expected<RefPtr<JSON::Object>, String> parseWebExtensionActionIconJSON(const String&);
}
#endif
