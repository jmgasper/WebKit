/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContentWorldType.h"
#include "WebExtensionFrameIdentifier.h"
#include "WebExtensionScriptInjectionParameters.h"
#include "WebExtensionTabParameters.h"
#include "WebExtensionWindowParameters.h"
#include <expected>
#include <wtf/JSONValues.h>
#include <wtf/URL.h>

namespace WebKit {

// C++ counterparts of the option parsers in WebExtensionAPITabsCocoa.mm and
// WebExtensionAPIWindowsCocoa.mm. They parse the request only: tab and window
// lookups, URL policy, permissions and the host browser's abilities belong to
// the UI process. Unknown members are ignored, as the Cocoa parsers do; known
// members must have the documented type. Errors are complete messages.
std::expected<WebExtensionTabParameters, String> parseWebExtensionTabCreateProperties(const JSON::Value*, const URL& baseURL);
std::expected<WebExtensionTabParameters, String> parseWebExtensionTabUpdateProperties(const JSON::Value*, const URL& baseURL);
std::expected<Vector<WebExtensionTabIdentifier>, String> parseWebExtensionTabIdentifiers(const JSON::Value*);
// True when the reload must bypass the cache. A missing value reloads normally.
std::expected<bool, String> parseWebExtensionTabReloadProperties(const JSON::Value*);
// tabs.insertCSS() and tabs.removeCSS() details.
std::expected<WebExtensionScriptInjectionParameters, String> parseWebExtensionTabCSSDetails(const JSON::Value*);

// 'left', 'top', 'width' and 'height' are validated and then dropped: the
// native window parameters carry no frame.
std::expected<WebExtensionWindowParameters, String> parseWebExtensionWindowCreateData(const JSON::Value*, const URL& baseURL);
std::expected<WebExtensionWindowParameters, String> parseWebExtensionWindowUpdateInfo(const JSON::Value*);

}
#endif
