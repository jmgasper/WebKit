/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "JSWebExtensionWindowParameters.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionString.h"
#include "JSWebExtensionTabParameters.h"
#include "Protected.h"
#include "WebExtensionWindowParameters.h"
namespace WebKit {
JSValueRef toWebAPI(JSContextRef context, const WebExtensionWindowParameters& parameters)
{
    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> result(globalContext, JSObjectMake(context, nullptr, nullptr));
    auto set = [&](ASCIILiteral key, JSValueRef value) {
        JSObjectSetProperty(context, result.get(), toJSString(key).get(), value, kJSPropertyAttributeNone, nullptr);
    };
    auto stringValue = [&](ASCIILiteral value) {
        return JSValueMakeString(context, toJSString(value).get());
    };
    if (parameters.identifier)
        set("id"_s, JSValueMakeNumber(context, toWebAPI(*parameters.identifier)));
    if (parameters.type)
        set("type"_s, stringValue(*parameters.type == WebExtensionWindow::Type::Normal ? "normal"_s : "popup"_s));
    if (parameters.state) {
        switch (*parameters.state) {
        case WebExtensionWindow::State::Normal: set("state"_s, stringValue("normal"_s)); break;
        case WebExtensionWindow::State::Minimized: set("state"_s, stringValue("minimized"_s)); break;
        case WebExtensionWindow::State::Maximized: set("state"_s, stringValue("maximized"_s)); break;
        case WebExtensionWindow::State::Fullscreen: set("state"_s, stringValue("fullscreen"_s)); break;
        }
    }
    if (parameters.focused)
        set("focused"_s, JSValueMakeBoolean(context, *parameters.focused));
    if (parameters.privateBrowsing)
        set("incognito"_s, JSValueMakeBoolean(context, *parameters.privateBrowsing));
    // Registered native browser windows have normal window feel.
    set("alwaysOnTop"_s, JSValueMakeBoolean(context, false));
    if (parameters.tabs) {
        Vector<Protected<JSValueRef>> protectedTabs;
        Vector<JSValueRef> tabs;
        for (auto& tab : *parameters.tabs) {
            protectedTabs.append(Protected<JSValueRef>(globalContext, toWebAPI(context, tab)));
            tabs.append(protectedTabs.last().get());
        }
        Protected<JSObjectRef> array(globalContext, JSObjectMakeArray(context, tabs.size(), tabs.span().data(), nullptr));
        set("tabs"_s, array.get());
    }
    return result.get();
}
}
#endif
