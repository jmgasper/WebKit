/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionAPIWebRequest.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "FormDataReference.h"
#include "Logging.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionWebRequestBody.h"
#include "WebExtensionWebRequestFilter.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/StrongInlines.h>
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/HTTPHeaderMap.h>
#include <WebCore/ProtectionSpace.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>

#if PLATFORM(HAIKU)
#include "JSWebExtensionWrapper.h"
#include "Protected.h"
#include "WebExtensionWebRequestGateResultHaiku.h"
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <wtf/RunLoop.h>
#endif

namespace WebKit {

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onBeforeRequest()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onBeforeRequest

    if (!m_onBeforeRequestEvent)
        m_onBeforeRequestEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnBeforeRequest);

    return *m_onBeforeRequestEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onBeforeSendHeaders()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onBeforeSendHeaders

    if (!m_onBeforeSendHeadersEvent)
        m_onBeforeSendHeadersEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders);

    return *m_onBeforeSendHeadersEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onSendHeaders()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onSendHeaders

    if (!m_onSendHeadersEvent)
        m_onSendHeadersEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnSendHeaders);

    return *m_onSendHeadersEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onHeadersReceived()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onHeadersReceived

    if (!m_onHeadersReceivedEvent)
        m_onHeadersReceivedEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnHeadersReceived);

    return *m_onHeadersReceivedEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onAuthRequired()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onAuthRequired

    if (!m_onAuthRequiredEvent)
        m_onAuthRequiredEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnAuthRequired);

    return *m_onAuthRequiredEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onBeforeRedirect()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onBeforeRedirect

    if (!m_onBeforeRedirectEvent)
        m_onBeforeRedirectEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnBeforeRedirect);

    return *m_onBeforeRedirectEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onResponseStarted()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onResponseStarted

    if (!m_onResponseStartedEvent)
        m_onResponseStartedEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnResponseStarted);

    return *m_onResponseStartedEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onCompleted()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onCompleted

    if (!m_onCompletedEvent)
        m_onCompletedEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnCompleted);

    return *m_onCompletedEvent;
}

WebExtensionAPIWebRequestEvent& WebExtensionAPIWebRequest::onErrorOccurred()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onErrorOccurred

    if (!m_onErrorOccurredEvent)
        m_onErrorOccurredEvent = WebExtensionAPIWebRequestEvent::create(*this, WebExtensionEventListenerType::WebRequestOnErrorOccurred);

    return *m_onErrorOccurredEvent;
}

static Ref<JSON::Array> webRequestHeaders(const WebCore::HTTPHeaderMap& headers)
{
    auto result = JSON::Array::create();
    for (auto& header : headers) {
        auto entry = JSON::Object::create();
        entry->setString("name"_s, header.key);
        entry->setString("value"_s, header.value);
        result->pushObject(WTF::move(entry));
    }
    return result;
}

static Ref<JSON::Object> webRequestResponseDetails(const ResourceLoadInfo& resource, WebExtensionTabIdentifier tab, const WebCore::ResourceResponse& response, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
    details->setInteger("statusCode"_s, response.httpStatusCode());
    details->setString("statusLine"_s, webExtensionWebRequestStatusLine(response.httpVersion(), response.httpStatusCode(), response.httpStatusText()));
    details->setBoolean("fromCache"_s, resource.loadedFromCache);
    return details;
}

#if PLATFORM(HAIKU)
// Observation deliveries skip blocking listeners that were already invoked by
// the blocking dispatch for the same load and phase; blocking dispatches invoke
// only blocking listeners and hand each call to the invoker.
enum class WebRequestListenerSelectionHaiku : uint8_t { All, NonBlocking, Blocking };
using WebRequestListenerInvokerHaiku = Function<void(WebExtensionCallbackHandler&, const Vector<String>& extraInfo, JSContextRef, JSObjectRef argument)>;
#endif

static void deliverWebRequest(WebExtensionAPIWebRequestEvent& event, const JSON::Object& details, WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const ResourceLoadInfo& resource,
    const String& headerKey = nullString(), RefPtr<JSON::Array> headers = nullptr, RefPtr<WebCore::FormData> body = nullptr, const String& contentType = nullString()
#if PLATFORM(HAIKU)
    , WebRequestListenerSelectionHaiku selection = WebRequestListenerSelectionHaiku::All, const WebRequestListenerInvokerHaiku* invoker = nullptr
#endif
    )
{
    auto serialized = details.toJSONString();
    event.enumerateListeners(tab, window, resource, [&](auto& listener, auto& extraInfo) {
#if PLATFORM(HAIKU)
        if (selection != WebRequestListenerSelectionHaiku::All && WebExtensionAPIWebRequestEvent::isBlockingListenerHaiku(extraInfo) != (selection == WebRequestListenerSelectionHaiku::Blocking))
            return;
#endif
        auto context = listener.globalContext();
        if (!context)
            return;
        // Both the JSON metadata and any buffers are fresh for each callback.
        auto payload = JSON::Value::parseJSON(serialized)->asObject();
        if (headers && extraInfo.contains(headerKey))
            payload->setArray(headerKey, Ref { *headers });
        bool includeBody = body && !body->isEmpty() && extraInfo.contains("requestBody"_s);
        if (includeBody)
            payload->setValue("requestBody"_s, JSON::Value::null());
        auto* globalObject = toJS(context);
        auto& vm = globalObject->vm();
        JSC::JSLockHolder lock(vm);
        JSValueRef exception = nullptr;
        auto argument = JSValueToObject(context, JSValueMakeFromJSONString(context, toJSString(payload->toJSONString()).get()), &exception);
        if (!argument || exception)
            return;
        JSC::Strong<JSC::Unknown> protectedArgument(vm, toJS(globalObject, argument));
        if (includeBody) {
            auto requestBody = webExtensionWebRequestBody(context, body->elements().span(), contentType, exception);
            if (!requestBody || exception) {
                RELEASE_LOG_ERROR(Extensions, "Unable to serialize webRequest upload body");
                return;
            }
            JSC::Strong<JSC::Unknown> protectedBody(vm, toJS(globalObject, requestBody));
            JSObjectSetProperty(context, argument, toJSString("requestBody"_s).get(), requestBody, kJSPropertyAttributeNone, &exception);
            if (exception)
                return;
        }
#if PLATFORM(HAIKU)
        if (invoker) {
            (*invoker)(listener, extraInfo, context, argument);
            return;
        }
#endif
        listener.call(argument);
    });
}

#if PLATFORM(HAIKU)
static WebRequestListenerSelectionHaiku observationSelectionHaiku(WebExtensionContextProxy& context, const ResourceLoadInfo& resource, uint8_t phase)
{
    return context.consumeBlockingWebRequestDeliveryHaiku(resource.resourceLoadID.toUInt64(), phase) ? WebRequestListenerSelectionHaiku::NonBlocking : WebRequestListenerSelectionHaiku::All;
}
#endif

void WebExtensionContextProxy::resourceLoadDidSendRequest(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const WebCore::ResourceRequest& request, const ResourceLoadInfo& resource, const std::optional<IPC::FormDataReference>& formData, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
    RefPtr body = formData ? formData->data() : nullptr;
    auto contentType = request.httpContentType();
#if PLATFORM(HAIKU)
    auto beforeRequestSelection = observationSelectionHaiku(*this, resource, webExtensionWebRequestBeforeRequestPhaseHaiku);
    auto beforeSendHeadersSelection = observationSelectionHaiku(*this, resource, webExtensionWebRequestBeforeSendHeadersPhaseHaiku);
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeRequest(), details, tab, window, resource, nullString(), nullptr, body, contentType, beforeRequestSelection);
    });
    auto headers = webRequestHeaders(request.httpHeaderFields());
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeSendHeaders(), details, tab, window, resource, "requestHeaders"_s, headers.copyRef(), nullptr, nullString(), beforeSendHeadersSelection);
        deliverWebRequest(object.webRequest().onSendHeaders(), details, tab, window, resource, "requestHeaders"_s, headers.copyRef());
    });
#else
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeRequest(), details, tab, window, resource, nullString(), nullptr, body, contentType);
    });
    auto headers = webRequestHeaders(request.httpHeaderFields());
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeSendHeaders(), details, tab, window, resource, "requestHeaders"_s, headers.copyRef());
        deliverWebRequest(object.webRequest().onSendHeaders(), details, tab, window, resource, "requestHeaders"_s, headers.copyRef());
    });
#endif
}

void WebExtensionContextProxy::resourceLoadDidBlockBeforeRequest(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const ResourceLoadInfo& resource, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeRequest(), details, tab, window, resource);
    });
}

void WebExtensionContextProxy::resourceLoadDidPerformHTTPRedirection(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const WebCore::ResourceResponse& response, const ResourceLoadInfo& resource, const WebCore::ResourceRequest& request, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto details = webRequestResponseDetails(resource, tab, response, mainFrameIdentifier);
    auto headers = webRequestHeaders(response.httpHeaderFields());
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onHeadersReceived(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
    details->setString("redirectUrl"_s, request.url().string());
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onBeforeRedirect(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
}

static String webRequestAuthenticationScheme(WebCore::ProtectionSpaceBaseAuthenticationScheme scheme)
{
    switch (scheme) {
    case WebCore::ProtectionSpaceBaseAuthenticationScheme::HTTPBasic: return "basic"_s;
    case WebCore::ProtectionSpaceBaseAuthenticationScheme::HTTPDigest: return "digest"_s;
    case WebCore::ProtectionSpaceBaseAuthenticationScheme::NTLM: return "ntlm"_s;
    case WebCore::ProtectionSpaceBaseAuthenticationScheme::Negotiate: return "negotiate"_s;
    default: return emptyString();
    }
}

void WebExtensionContextProxy::resourceLoadDidReceiveChallenge(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const WebCore::AuthenticationChallenge& challenge, const ResourceLoadInfo& resource, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto& response = challenge.failureResponse();
    if (response.httpStatusCode() != 401 && response.httpStatusCode() != 407)
        return;
    auto details = webRequestResponseDetails(resource, tab, response, mainFrameIdentifier);
    auto& protectionSpace = challenge.protectionSpace();
    details->setString("scheme"_s, webRequestAuthenticationScheme(protectionSpace.authenticationScheme()));
    auto challenger = JSON::Object::create();
    challenger->setString("host"_s, protectionSpace.host());
    challenger->setInteger("port"_s, protectionSpace.port());
    details->setObject("challenger"_s, WTF::move(challenger));
    details->setBoolean("isProxy"_s, protectionSpace.isProxy());
    if (!protectionSpace.realm().isNull())
        details->setString("realm"_s, protectionSpace.realm());
    auto headers = webRequestHeaders(response.httpHeaderFields());
#if PLATFORM(HAIKU)
    auto selection = observationSelectionHaiku(*this, resource, webExtensionWebRequestAuthRequiredPhaseHaiku);
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onAuthRequired(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef(), nullptr, nullString(), selection);
    });
#else
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onAuthRequired(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
#endif
}

void WebExtensionContextProxy::resourceLoadDidReceiveResponse(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const WebCore::ResourceResponse& response, const ResourceLoadInfo& resource, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto details = webRequestResponseDetails(resource, tab, response, mainFrameIdentifier);
    auto headers = webRequestHeaders(response.httpHeaderFields());
#if PLATFORM(HAIKU)
    auto selection = observationSelectionHaiku(*this, resource, webExtensionWebRequestHeadersReceivedPhaseHaiku);
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onHeadersReceived(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef(), nullptr, nullString(), selection);
        deliverWebRequest(object.webRequest().onResponseStarted(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
#else
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onHeadersReceived(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
        deliverWebRequest(object.webRequest().onResponseStarted(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
#endif
}

void WebExtensionContextProxy::resourceLoadDidCompleteWithError(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const WebCore::ResourceResponse& response, const WebCore::ResourceError& error, const ResourceLoadInfo& resource, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
#if PLATFORM(HAIKU)
    forgetBlockingWebRequestDeliveriesHaiku(resource.resourceLoadID.toUInt64());
#endif
    if (!error.isNull()) {
        auto details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
        details->setString("error"_s, error.localizedDescription());
        enumerateNamespaceObjects([&](auto& object) {
            deliverWebRequest(object.webRequest().onErrorOccurred(), details, tab, window, resource);
        });
        return;
    }
    auto details = webRequestResponseDetails(resource, tab, response, mainFrameIdentifier);
    auto headers = webRequestHeaders(response.httpHeaderFields());
    enumerateNamespaceObjects([&](auto& object) {
        deliverWebRequest(object.webRequest().onCompleted(), details, tab, window, resource, "responseHeaders"_s, headers.copyRef());
    });
}

#if PLATFORM(HAIKU)
namespace {

// Collects one reply per invoked blocking listener. Synchronous results settle
// immediately; Promise and asyncBlocking results settle later or are dropped
// (no decision) when the listener deadline passes. The completion handler runs
// exactly once, at the latest when the collector is destroyed.
class BlockingWebRequestReplies : public RefCounted<BlockingWebRequestReplies> {
public:
    static Ref<BlockingWebRequestReplies> create(CompletionHandler<void(Vector<String>&&)>&& completionHandler)
    {
        return adoptRef(*new BlockingWebRequestReplies(WTF::move(completionHandler)));
    }

    ~BlockingWebRequestReplies()
    {
        complete();
    }

    size_t reserve()
    {
        m_replies.append(String { });
        m_settled.append(false);
        ++m_pending;
        return m_replies.size() - 1;
    }

    void settle(size_t slot, String&& reply)
    {
        if (m_completed || slot >= m_replies.size() || m_settled[slot])
            return;
        m_settled[slot] = true;
        m_replies[slot] = WTF::move(reply);
        if (!--m_pending && m_dispatchFinished)
            complete();
    }

    void dispatchFinished()
    {
        m_dispatchFinished = true;
        if (!m_pending)
            complete();
        else if (!m_completed)
            m_deadline.startOneShot(webExtensionWebRequestListenerTimeoutHaiku);
    }

private:
    explicit BlockingWebRequestReplies(CompletionHandler<void(Vector<String>&&)>&& completionHandler)
        : m_completionHandler(WTF::move(completionHandler))
        , m_deadline(RunLoop::mainSingleton(), "WebExtensionWebRequestListenerDeadline"_s, [this] {
            RELEASE_LOG_ERROR(Extensions, "A blocking webRequest listener did not settle in time; treating it as no decision");
            complete();
        })
    {
    }

    void complete()
    {
        if (m_completed)
            return;
        m_completed = true;
        m_deadline.stop();
        m_completionHandler(WTF::move(m_replies));
    }

    CompletionHandler<void(Vector<String>&&)> m_completionHandler;
    Vector<String> m_replies;
    Vector<bool> m_settled;
    size_t m_pending { 0 };
    bool m_dispatchFinished { false };
    bool m_completed { false };
    RunLoop::Timer m_deadline;
};

// A BlockingResponse as JSON. Empty means "no decision" (undefined, null,
// exceptions and unserializable values); anything else is decoded, and
// rejected if invalid, by the UI process.
String blockingReplyJSON(JSContextRef context, JSValueRef value)
{
    if (!context || !value || JSValueIsUndefined(context, value) || JSValueIsNull(context, value))
        return { };
    JSValueRef exception = nullptr;
    auto json = serializeJSObject(context, value, &exception);
    static constexpr unsigned maximumReplyLength = 1024 * 1024;
    if (exception || json.isNull() || json.length() > maximumReplyLength)
        return { };
    return json;
}

JSObjectRef makeSettleFunction(JSContextRef context, Ref<BlockingWebRequestReplies>&& replies, size_t slot, bool isRejection)
{
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    auto* function = JSC::JSNativeStdFunction::create(vm, globalObject, 1, String(), [replies = WTF::move(replies), slot, isRejection](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) {
        // A rejected Promise is an error, never a network mutation.
        replies->settle(slot, isRejection ? String { } : blockingReplyJSON(toRef(globalObject), toRef(globalObject, callFrame->argument(0))));
        return JSC::JSValue::encode(JSC::jsUndefined());
    });
    return toRef(function);
}

void settleWhenThenableSettles(JSContextRef context, JSValueRef thenable, BlockingWebRequestReplies& replies, size_t slot)
{
    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSValueRef> protectedThenable(globalContext, thenable);
    JSValueRef exception = nullptr;
    auto thisObject = JSValueToObject(context, thenable, &exception);
    if (!thisObject || exception)
        return replies.settle(slot, { });

    JSRetainPtr thenName(Adopt, JSStringCreateWithUTF8CString("then"));
    auto method = JSObjectGetProperty(context, thisObject, thenName.get(), &exception);
    auto thenFunction = method && !exception ? JSValueToObject(context, method, &exception) : nullptr;
    if (!thenFunction || exception)
        return replies.settle(slot, { });

    Protected<JSValueRef> resolveFunction(globalContext, makeSettleFunction(context, Ref { replies }, slot, false));
    Protected<JSValueRef> rejectFunction(globalContext, makeSettleFunction(context, Ref { replies }, slot, true));
    std::array<JSValueRef, 2> arguments { resolveFunction.get(), rejectFunction.get() };
    JSObjectCallAsFunction(context, thenFunction, thisObject, arguments.size(), arguments.data(), &exception);
    if (exception)
        replies.settle(slot, { });
}

WebExtensionAPIWebRequestEvent* blockingEventForPhase(WebExtensionAPIWebRequest& webRequest, uint8_t phase)
{
    switch (phase) {
    case webExtensionWebRequestBeforeRequestPhaseHaiku:
        return &webRequest.onBeforeRequest();
    case webExtensionWebRequestBeforeSendHeadersPhaseHaiku:
        return &webRequest.onBeforeSendHeaders();
    case webExtensionWebRequestHeadersReceivedPhaseHaiku:
        return &webRequest.onHeadersReceived();
    case webExtensionWebRequestAuthRequiredPhaseHaiku:
        return &webRequest.onAuthRequired();
    default:
        return nullptr;
    }
}

}

void WebExtensionContextProxy::dispatchBlockingWebRequestHaiku(uint8_t phase, WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const ResourceLoadInfo& resource, const WebCore::ResourceRequest& request,
    const std::optional<IPC::FormDataReference>& formData, const std::optional<WebCore::ResourceResponse>& response, const std::optional<WebCore::AuthenticationChallenge>& challenge,
    std::optional<WebCore::FrameIdentifier> mainFrameIdentifier, CompletionHandler<void(Vector<String>&&)>&& completionHandler)
{
    if (phase > webExtensionWebRequestLastPhaseHaiku
        || ((phase == webExtensionWebRequestHeadersReceivedPhaseHaiku || phase == webExtensionWebRequestAuthRequiredPhaseHaiku) && !response)
        || (phase == webExtensionWebRequestAuthRequiredPhaseHaiku && !challenge))
        return completionHandler({ });

    // The observation of this load/phase that follows must not invoke the
    // blocking listeners a second time.
    noteBlockingWebRequestDeliveryHaiku(resource.resourceLoadID.toUInt64(), phase);

    // Build the same details the observation path would.
    RefPtr<JSON::Object> details;
    RefPtr<JSON::Array> headers;
    String headerKey;
    RefPtr<WebCore::FormData> body;
    String contentType;
    switch (phase) {
    case webExtensionWebRequestBeforeRequestPhaseHaiku:
        details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
        body = formData ? formData->data() : nullptr;
        contentType = request.httpContentType();
        break;
    case webExtensionWebRequestBeforeSendHeadersPhaseHaiku:
        details = webExtensionWebRequestDetails(resource, tab, mainFrameIdentifier);
        headerKey = "requestHeaders"_s;
        headers = webRequestHeaders(request.httpHeaderFields());
        break;
    case webExtensionWebRequestHeadersReceivedPhaseHaiku:
        details = webRequestResponseDetails(resource, tab, *response, mainFrameIdentifier);
        headerKey = "responseHeaders"_s;
        headers = webRequestHeaders(response->httpHeaderFields());
        break;
    case webExtensionWebRequestAuthRequiredPhaseHaiku: {
        details = webRequestResponseDetails(resource, tab, *response, mainFrameIdentifier);
        auto& protectionSpace = challenge->protectionSpace();
        details->setString("scheme"_s, webRequestAuthenticationScheme(protectionSpace.authenticationScheme()));
        auto challenger = JSON::Object::create();
        challenger->setString("host"_s, protectionSpace.host());
        challenger->setInteger("port"_s, protectionSpace.port());
        details->setObject("challenger"_s, WTF::move(challenger));
        details->setBoolean("isProxy"_s, protectionSpace.isProxy());
        if (!protectionSpace.realm().isNull())
            details->setString("realm"_s, protectionSpace.realm());
        headerKey = "responseHeaders"_s;
        headers = webRequestHeaders(response->httpHeaderFields());
        break;
    }
    }

    auto replies = BlockingWebRequestReplies::create(WTF::move(completionHandler));
    WebRequestListenerInvokerHaiku invoker = [&](WebExtensionCallbackHandler& listener, const Vector<String>& extraInfo, JSContextRef context, JSObjectRef argument) {
        auto slot = replies->reserve();
        if (extraInfo.contains("asyncBlocking"_s)) {
            // Chrome's asyncBlocking: the listener answers through a callback.
            Protected<JSValueRef> callback(JSContextGetGlobalContext(context), makeSettleFunction(context, replies.copyRef(), slot, false));
            JSValueRef result = listener.call(argument, callback.get());
            if (result && isThenable(context, result))
                settleWhenThenableSettles(context, result, replies, slot);
            return;
        }
        // blocking: the return value, or (as in Firefox) a Promise for it.
        JSValueRef result = listener.call(argument);
        if (result && isThenable(context, result)) {
            settleWhenThenableSettles(context, result, replies, slot);
            return;
        }
        replies->settle(slot, blockingReplyJSON(context, result));
    };

    enumerateNamespaceObjects([&](auto& object) {
        if (auto* event = blockingEventForPhase(object.webRequest(), phase))
            deliverWebRequest(*event, *details, tab, window, resource, headerKey, headers, body, contentType, WebRequestListenerSelectionHaiku::Blocking, &invoker);
    });
    replies->dispatchFinished();
}

void WebExtensionContextProxy::noteBlockingWebRequestDeliveryHaiku(uint64_t resourceLoadID, uint8_t phase)
{
    // Bounded: loads cancelled by an extension never produce the observation.
    static constexpr size_t maximumRememberedDeliveries = 512;
    if (m_blockingWebRequestDeliveriesHaiku.size() >= maximumRememberedDeliveries)
        m_blockingWebRequestDeliveriesHaiku.removeAt(0);
    m_blockingWebRequestDeliveriesHaiku.append({ resourceLoadID, phase });
}

bool WebExtensionContextProxy::consumeBlockingWebRequestDeliveryHaiku(uint64_t resourceLoadID, uint8_t phase)
{
    auto index = m_blockingWebRequestDeliveriesHaiku.reverseFind(std::pair { resourceLoadID, phase });
    if (index == notFound)
        return false;
    m_blockingWebRequestDeliveriesHaiku.removeAt(index);
    return true;
}

void WebExtensionContextProxy::forgetBlockingWebRequestDeliveriesHaiku(uint64_t resourceLoadID)
{
    m_blockingWebRequestDeliveriesHaiku.removeAllMatching([&](auto& entry) {
        return entry.first == resourceLoadID;
    });
}
#endif

// Chrome's extraInfoSpec and ResourceType enum objects. Violentmonkey evaluates
// `browser.webRequest.OnBeforeSendHeadersOptions.EXTRA_HEADERS` at the top level of
// its background script; without these objects the whole background failed.
static RefPtr<JSON::Object> webRequestEnum(std::initializer_list<std::pair<ASCIILiteral, ASCIILiteral>> members)
{
    Ref object = JSON::Object::create();
    for (auto& [name, value] : members)
        object->setString(name, value);
    return object;
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onBeforeRequestOptions()
{
    return webRequestEnum({ { "BLOCKING"_s, "blocking"_s }, { "REQUEST_BODY"_s, "requestBody"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onBeforeSendHeadersOptions()
{
    return webRequestEnum({ { "REQUEST_HEADERS"_s, "requestHeaders"_s }, { "BLOCKING"_s, "blocking"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onSendHeadersOptions()
{
    return webRequestEnum({ { "REQUEST_HEADERS"_s, "requestHeaders"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onHeadersReceivedOptions()
{
    return webRequestEnum({ { "BLOCKING"_s, "blocking"_s }, { "RESPONSE_HEADERS"_s, "responseHeaders"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onAuthRequiredOptions()
{
    return webRequestEnum({ { "RESPONSE_HEADERS"_s, "responseHeaders"_s }, { "BLOCKING"_s, "blocking"_s }, { "ASYNC_BLOCKING"_s, "asyncBlocking"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onResponseStartedOptions()
{
    return webRequestEnum({ { "RESPONSE_HEADERS"_s, "responseHeaders"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onBeforeRedirectOptions()
{
    return webRequestEnum({ { "RESPONSE_HEADERS"_s, "responseHeaders"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onCompletedOptions()
{
    return webRequestEnum({ { "RESPONSE_HEADERS"_s, "responseHeaders"_s }, { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::onErrorOccurredOptions()
{
    return webRequestEnum({ { "EXTRA_HEADERS"_s, "extraHeaders"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIWebRequest::resourceTypes()
{
    return webRequestEnum({ { "MAIN_FRAME"_s, "main_frame"_s }, { "SUB_FRAME"_s, "sub_frame"_s }, { "STYLESHEET"_s, "stylesheet"_s },
        { "SCRIPT"_s, "script"_s }, { "IMAGE"_s, "image"_s }, { "FONT"_s, "font"_s }, { "OBJECT"_s, "object"_s },
        { "XMLHTTPREQUEST"_s, "xmlhttprequest"_s }, { "PING"_s, "ping"_s }, { "CSP_REPORT"_s, "csp_report"_s },
        { "MEDIA"_s, "media"_s }, { "WEBSOCKET"_s, "websocket"_s }, { "WEBBUNDLE"_s, "webbundle"_s }, { "OTHER"_s, "other"_s } });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
