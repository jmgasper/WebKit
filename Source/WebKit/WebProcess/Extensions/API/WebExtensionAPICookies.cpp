/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPICookies.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionCookieParameters.h"
#include "WebExtensionAPINamespace.h"
#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionCookieQueryParser.h"
#include "WebExtensionCookieWriteParser.h"
#include "WebProcess.h"
#include "WebExtensionPermission.h"
namespace WebKit {
static void callWithCookie(WebExtensionCallbackHandler& callback, std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    auto context = callback.globalContext();
    Protected<JSValueRef> value(context, *result ? toWebAPI(context, **result) : JSValueMakeNull(context));
    callback.call(value.get());
}
void WebExtensionAPICookies::get(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto query = parseWebExtensionCookieQuery(details.get(), WebExtensionCookieQueryKind::Get);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CookiesGet(query->session, *query->filter.name, query->url),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](auto&& result) {
            callWithCookie(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}
void WebExtensionAPICookies::getAll(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto query = parseWebExtensionCookieQuery(details.get(), WebExtensionCookieQueryKind::GetAll);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CookiesGetAll(query->session, query->url, query->filter),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, toWebAPI(context, *result));
            callback->call(value.get());
        }, extensionContext().identifier());
}
void WebExtensionAPICookies::set(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto parsed = parseWebExtensionCookieWrite(details.get());
    if (!parsed) {
        exception = parsed.error();
        return;
    }
    // Send the original details, including URL, for independent UI validation.
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CookiesSetJSON(details->toJSONString()),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](auto&& result) {
            callWithCookie(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}
void WebExtensionAPICookies::remove(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto query = parseWebExtensionCookieQuery(details.get(), WebExtensionCookieQueryKind::Get);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CookiesRemove(query->session, *query->filter.name, query->url),
        [protectedThis = Ref { *this }, callback = WTF::move(callback), url = query->url](std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, *result ? toWebAPICookieRemoval(context, url, **result) : JSValueMakeNull(context));
            callback->call(value.get());
        }, extensionContext().identifier());
}
void WebExtensionAPICookies::getAllCookieStores(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CookiesGetAllCookieStores(),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, toWebAPI(context, *result, protectedThis->extensionContext().defaultSessionID()));
            callback->call(value.get());
        }, extensionContext().identifier());
}
WebExtensionAPIEvent& WebExtensionAPICookies::onChanged()
{
    if (!m_onChanged)
        m_onChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::CookiesOnChanged);
    return *m_onChanged;
}
void WebExtensionContextProxy::dispatchCookiesChangedEvent(PAL::SessionID sessionID, const Vector<WebCore::CookieChange>& changes)
{
    if (!sessionID.isValid() || !hasPermission(WebExtensionPermission::cookies()))
        return;
    // Deliver each committed batch in order. The factory allocates a new
    // changeInfo and nested cookie for every listener and JavaScript context.
    for (auto& change : changes) {
        enumerateFramesAndNamespaceObjects([&](auto&, auto& object) {
            if (!hasPermission(WebExtensionPermission::cookies()))
                return;
            object.cookies().onChanged().template invokeListenersWithArgumentFactory<1>([&](JSGlobalContextRef context) {
                return std::array { Protected<JSValueRef>(context, toWebAPICookieChange(context, sessionID, change)) };
            });
        });
    }
}
}
#endif
