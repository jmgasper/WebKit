/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionMenuItemParser.h"
#include "WebExtensionMenuItemContextParameters.h"
#include "Protected.h"
#include <JavaScriptCore/JavaScript.h>
namespace WebKit {
struct WebExtensionMenuItemInput {
    WebExtensionMenuItemParameters parameters;
    Protected<JSObjectRef> clickHandler;
    bool explicitIdentifier { false };
    bool clickHandlerSpecified { false };
};
std::expected<WebExtensionMenuItemInput, String> parseWebExtensionMenuItemInput(JSContextRef, JSValueRef, WebExtensionMenuItemOperation, bool manifestVersion3, const URL& baseURL);
std::expected<String, String> parseWebExtensionMenuIdentifier(JSContextRef, JSValueRef);
JSValueRef toWebAPIMenuIdentifier(JSContextRef, const String& nativeKey);
JSValueRef toWebAPIMenuClickInfo(JSContextRef, const WebExtensionMenuItemParameters&, bool wasChecked, const WebExtensionMenuItemContextParameters&);
}
#endif
