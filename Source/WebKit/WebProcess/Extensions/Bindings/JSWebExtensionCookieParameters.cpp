/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "JSWebExtensionCookieParameters.h"
#if ENABLE(WK_WEB_EXTENSIONS)
#include "JSWebExtensionString.h"
#include "Protected.h"
#include "WebExtensionCookieStoreIdentifier.h"
#include <cmath>
namespace WebKit {
JSValueRef toWebAPICookieChange(JSContextRef context, PAL::SessionID session, const WebCore::CookieChange& change)
{
    if (!session.isValid() || !change.cookie.partitionKey.isEmpty())
        return JSValueMakeNull(context);
    ASCIILiteral cause;
    switch (change.cause) {
    case WebCore::CookieChangeCause::Explicit: cause = "explicit"_s; break;
    case WebCore::CookieChangeCause::Overwrite: cause = "overwrite"_s; break;
    case WebCore::CookieChangeCause::Expired: cause = "expired"_s; break;
    case WebCore::CookieChangeCause::ExpiredOverwrite: cause = "expired_overwrite"_s; break;
    default: return JSValueMakeNull(context);
    }
    if (!change.removed && change.cause != WebCore::CookieChangeCause::Explicit)
        return JSValueMakeNull(context);
    auto global = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> result(global, JSObjectMake(context, nullptr, nullptr));
    Protected<JSValueRef> cookie(global, toWebAPI(context, WebExtensionCookieParameters { session, change.cookie }));
    JSObjectSetProperty(context, result.get(), toJSString("cookie"_s).get(), cookie.get(), kJSPropertyAttributeNone, nullptr);
    JSObjectSetProperty(context, result.get(), toJSString("removed"_s).get(), JSValueMakeBoolean(context, change.removed), kJSPropertyAttributeNone, nullptr);
    JSObjectSetProperty(context, result.get(), toJSString("cause"_s).get(), JSValueMakeString(context, toJSString(cause).get()), kJSPropertyAttributeNone, nullptr);
    return result.get();
}
JSValueRef toWebAPICookieRemoval(JSContextRef context, const URL& url, const WebExtensionCookieParameters& parameters)
{
    if (!parameters.sessionIdentifier || !parameters.sessionIdentifier->isValid())
        return JSValueMakeNull(context);
    Protected<JSObjectRef> result(JSContextGetGlobalContext(context), JSObjectMake(context, nullptr, nullptr));
    auto set = [&](ASCIILiteral key, const String& value) {
        JSObjectSetProperty(context, result.get(), toJSString(key).get(), JSValueMakeString(context, toJSString(value).get()), kJSPropertyAttributeNone, nullptr);
    };
    set("url"_s, url.string());
    set("name"_s, parameters.cookie.name);
    set("storeId"_s, webExtensionCookieStoreIdentifier(*parameters.sessionIdentifier));
    return result.get();
}
JSValueRef toWebAPI(JSContextRef context, const WebExtensionCookieParameters& parameters)
{
    if (!parameters.sessionIdentifier || !parameters.sessionIdentifier->isValid())
        return JSValueMakeNull(context);
    auto& cookie = parameters.cookie;
    Protected<JSObjectRef> result(JSContextGetGlobalContext(context), JSObjectMake(context, nullptr, nullptr));
    auto set = [&](ASCIILiteral key, JSValueRef value) {
        JSObjectSetProperty(context, result.get(), toJSString(key).get(), value, kJSPropertyAttributeNone, nullptr);
    };
    auto string = [&](const String& value) { return JSValueMakeString(context, toJSString(value).get()); };
    set("domain"_s, string(cookie.domain));
    set("hostOnly"_s, JSValueMakeBoolean(context, !cookie.domain.startsWith('.')));
    set("httpOnly"_s, JSValueMakeBoolean(context, cookie.httpOnly));
    set("name"_s, string(cookie.name));
    set("path"_s, string(cookie.path));
    ASCIILiteral sameSite = "no_restriction"_s;
    switch (cookie.sameSite) {
    case WebCore::Cookie::SameSitePolicy::None: break;
    case WebCore::Cookie::SameSitePolicy::Lax: sameSite = "lax"_s; break;
    case WebCore::Cookie::SameSitePolicy::Strict: sameSite = "strict"_s; break;
    }
    set("sameSite"_s, string(sameSite));
    set("secure"_s, JSValueMakeBoolean(context, cookie.secure));
    set("session"_s, JSValueMakeBoolean(context, cookie.session));
    set("storeId"_s, string(webExtensionCookieStoreIdentifier(*parameters.sessionIdentifier)));
    set("value"_s, string(cookie.value));
    if (!cookie.session && cookie.expires && std::isfinite(*cookie.expires))
        set("expirationDate"_s, JSValueMakeNumber(context, *cookie.expires / 1000.0));
    return result.get();
}
JSValueRef toWebAPI(JSContextRef context, const Vector<WebExtensionCookieParameters>& cookies)
{
    auto global = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> result(global, JSObjectMakeArray(context, 0, nullptr, nullptr));
    unsigned index = 0;
    for (auto& cookie : cookies) {
        Protected<JSValueRef> value(global, toWebAPI(context, cookie));
        JSObjectSetPropertyAtIndex(context, result.get(), index++, value.get(), nullptr);
    }
    return result.get();
}
JSValueRef toWebAPI(JSContextRef context, const HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>>& stores, PAL::SessionID ownSession)
{
    auto global = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> result(global, JSObjectMakeArray(context, 0, nullptr, nullptr));
    unsigned index = 0;
    for (auto& entry : stores) {
        Protected<JSObjectRef> store(global, JSObjectMake(context, nullptr, nullptr));
        Protected<JSObjectRef> tabs(global, JSObjectMakeArray(context, 0, nullptr, nullptr));
        unsigned tabIndex = 0;
        for (auto tab : entry.value)
            JSObjectSetPropertyAtIndex(context, tabs.get(), tabIndex++, JSValueMakeNumber(context, toWebAPI(tab)), nullptr);
        JSObjectSetProperty(context, store.get(), toJSString("id"_s).get(), JSValueMakeString(context, toJSString(webExtensionCookieStoreIdentifier(entry.key)).get()), kJSPropertyAttributeNone, nullptr);
        JSObjectSetProperty(context, store.get(), toJSString("tabIds"_s).get(), tabs.get(), kJSPropertyAttributeNone, nullptr);
        JSObjectSetProperty(context, store.get(), toJSString("incognito"_s).get(), JSValueMakeBoolean(context, entry.key.isEphemeral() && entry.key != ownSession), kJSPropertyAttributeNone, nullptr);
        JSObjectSetPropertyAtIndex(context, result.get(), index++, store.get(), nullptr);
    }
    return result.get();
}
}
#endif
