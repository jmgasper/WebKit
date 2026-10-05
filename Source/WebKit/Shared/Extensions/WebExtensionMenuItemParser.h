/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <expected>
#include <variant>
#include <wtf/JSONValues.h>
#include <wtf/OptionSet.h>
#include <wtf/URL.h>
#include "WebExtensionMenuItemParameters.h"
namespace WebKit {
enum class WebExtensionMenuItemOperation : bool { Create, Update };
using WebExtensionMenuIdentifier = std::variant<String, int64_t>;
// Native menu maps and IPC carry a tagged key, so string "1" never aliases 1.
std::expected<String, String> encodeWebExtensionMenuIdentifier(const JSON::Value*);
std::expected<WebExtensionMenuIdentifier, String> decodeWebExtensionMenuIdentifier(const String&);
std::expected<WebExtensionMenuItemParameters, String> parseWebExtensionMenuItemProperties(const JSON::Value*, WebExtensionMenuItemOperation, bool manifestVersion3, const URL& baseURL);
std::expected<void, String> validateWebExtensionMenuItemParameters(const WebExtensionMenuItemParameters&, WebExtensionMenuItemOperation);
}
#endif
