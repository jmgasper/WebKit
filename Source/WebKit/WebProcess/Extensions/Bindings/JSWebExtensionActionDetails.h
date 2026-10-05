/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS)
#include "WebExtensionTabIdentifier.h"
#include <JavaScriptCore/JSValueRef.h>
#include <expected>
namespace WebKit {
inline std::expected<std::optional<WebExtensionTabIdentifier>, String> parseWebExtensionActionTabIdentifier(JSContextRef context, JSValueRef value)
{
    if (!value || JSValueIsUndefined(context, value))
        return std::optional<WebExtensionTabIdentifier> { };
    if (!JSValueIsNumber(context, value))
        return std::unexpected("tabId must be a valid safe integer identifier"_s);
    auto number = JSValueToNumber(context, value, nullptr);
    auto tab = toWebExtensionTabIdentifier(number);
    if (!isValid(tab) || number > 9007199254740991.0)
        return std::unexpected("tabId must be a valid safe integer identifier"_s);
    return tab;
}
}
#endif
