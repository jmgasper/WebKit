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
#include "WebExtensionAPIWebRequestEvent.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebProcess.h"

namespace WebKit {

#if PLATFORM(HAIKU)
bool WebExtensionAPIWebRequestEvent::isBlockingListenerHaiku(const Vector<String>& extraInfo)
{
    return extraInfo.contains("blocking"_s) || extraInfo.contains("asyncBlocking"_s);
}

static bool NODELETE supportsBlockingHaiku(WebExtensionEventListenerType type)
{
    return type == WebExtensionEventListenerType::WebRequestOnBeforeRequest
        || type == WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders
        || type == WebExtensionEventListenerType::WebRequestOnHeadersReceived
        || type == WebExtensionEventListenerType::WebRequestOnAuthRequired;
}
#endif

void WebExtensionAPIWebRequestEvent::enumerateListeners(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const ResourceLoadInfo& resource, NOESCAPE const Function<void(WebExtensionCallbackHandler&, const Vector<String>&)>& function)
{
    auto snapshot = m_listeners;
    for (auto& listener : snapshot) {
        if (listener.filter && !listener.filter->matches(resource, tab, window))
            continue;
        function(Ref { *listener.callback }, listener.extraInfo);
    }
}

void WebExtensionAPIWebRequestEvent::invokeListenersWithJSONArgument(const String& argument, WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, const ResourceLoadInfo& resource)
{
    enumerateListeners(tab, window, resource, [&](auto& listener, auto&) {
        if (auto context = listener.globalContext())
            listener.call(JSValueMakeFromJSONString(context, toJSString(argument).get()));
    });
}

void WebExtensionAPIWebRequestEvent::addListener(WebCore::FrameIdentifier frame, RefPtr<WebExtensionCallbackHandler> listener, RefPtr<JSON::Value> filter, RefPtr<JSON::Value> extraInfoValue, String& outExceptionString)
{
    std::optional<WebExtensionWebRequestFilter> parsedFilter;
    if (filter && !filter->isNull()) {
        auto parsed = WebExtensionWebRequestFilter::parse(*filter);
        if (!parsed) {
            outExceptionString = parsed.error();
            return;
        }
        parsedFilter = WTF::move(*parsed);
    }
    Vector<String> extraInfo;
    if (extraInfoValue && !extraInfoValue->isNull()) {
        RefPtr array = extraInfoValue->asArray();
        if (!array) {
            outExceptionString = "The webRequest extraInfoSpec must be an array of strings."_s;
            return;
        }
        for (auto& value : *array) {
            auto name = value->asString();
#if PLATFORM(HAIKU)
            // Blocking listeners hold the request until they answer (see
            // WebExtensionWebRequestGateResultHaiku.h). extraHeaders only
            // changes which headers Chrome exposes; every header is exposed here.
            if (name == "blocking"_s || name == "asyncBlocking"_s) {
                bool supported = name == "asyncBlocking"_s ? m_type == WebExtensionEventListenerType::WebRequestOnAuthRequired : supportsBlockingHaiku(m_type);
                if (!supported) {
                    outExceptionString = "The requested extraInfoSpec is not supported for this webRequest event."_s;
                    return;
                }
                // Chrome MV3 grants onAuthRequired's blocking modes through
                // webRequestAuthProvider; MV3 packages cannot request
                // webRequestBlocking. Throwing here aborted LastPass' and
                // Bitwarden's MV3 background start-up.
                bool authProvider = m_type == WebExtensionEventListenerType::WebRequestOnAuthRequired && extensionContext().hasPermission("webRequestAuthProvider"_s);
                if (!extensionContext().hasPermission("webRequestBlocking"_s) && !authProvider) {
                    outExceptionString = "Using webRequest blocking listeners requires the webRequestBlocking permission."_s;
                    return;
                }
                if (!extraInfo.contains(name))
                    extraInfo.append(name);
                continue;
            }
            if (name == "extraHeaders"_s)
                continue;
#else
            // Like Safari, blocking registrations are accepted and delivered as observations;
            // listener return values are not applied to the network request yet.
            if (name == "blocking"_s || name == "asyncBlocking"_s || name == "extraHeaders"_s)
                continue;
#endif
            if (name != "requestBody"_s && name != "requestHeaders"_s && name != "responseHeaders"_s) {
                outExceptionString = "This webRequest implementation supports requestBody, requestHeaders and responseHeaders observation only."_s;
                return;
            }
            bool supportedForEvent = name == "requestBody"_s ? m_type == WebExtensionEventListenerType::WebRequestOnBeforeRequest
                : name == "requestHeaders"_s ? (m_type == WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders || m_type == WebExtensionEventListenerType::WebRequestOnSendHeaders)
                : (m_type == WebExtensionEventListenerType::WebRequestOnHeadersReceived || m_type == WebExtensionEventListenerType::WebRequestOnBeforeRedirect
                    || m_type == WebExtensionEventListenerType::WebRequestOnAuthRequired || m_type == WebExtensionEventListenerType::WebRequestOnResponseStarted
                    || m_type == WebExtensionEventListenerType::WebRequestOnCompleted);
            if (!supportedForEvent) {
                outExceptionString = "The requested extraInfoSpec is not supported for this webRequest event."_s;
                return;
            }
            if (!extraInfo.contains(name))
                extraInfo.append(name);
        }
    }
#if PLATFORM(HAIKU)
    if (extraInfo.contains("blocking"_s) && extraInfo.contains("asyncBlocking"_s)) {
        outExceptionString = "The blocking and asyncBlocking extraInfoSpec values are mutually exclusive."_s;
        return;
    }
    bool isBlocking = isBlockingListenerHaiku(extraInfo);
#endif
    m_frameIdentifier = frame;
    m_listeners.append({ WTF::move(listener), WTF::move(parsedFilter), WTF::move(extraInfo) });
    WebProcess::singleton().send(Messages::WebExtensionContext::AddListener(frame, m_type, contentWorldType()), extensionContext().identifier());
#if PLATFORM(HAIKU)
    if (isBlocking)
        WebProcess::singleton().send(Messages::WebExtensionContext::AddBlockingWebRequestListenerHaiku(frame, m_type), extensionContext().identifier());
#endif
}

void WebExtensionAPIWebRequestEvent::removeListener(WebCore::FrameIdentifier frameIdentifier, RefPtr<WebExtensionCallbackHandler> listener)
{
#if PLATFORM(HAIKU)
    uint64_t removedBlockingCount = 0;
    for (auto& entry : m_listeners) {
        if (entry.callback->callbackFunction() == listener->callbackFunction() && isBlockingListenerHaiku(entry.extraInfo))
            ++removedBlockingCount;
    }
#endif
    auto removedCount = m_listeners.removeAllMatching([&](auto& entry) {
        return entry.callback->callbackFunction() == listener->callbackFunction();
    });

    if (!removedCount)
        return;

    ASSERT(frameIdentifier == m_frameIdentifier);

    WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(*m_frameIdentifier, m_type, contentWorldType(), removedCount), extensionContext().identifier());
#if PLATFORM(HAIKU)
    if (removedBlockingCount)
        WebProcess::singleton().send(Messages::WebExtensionContext::RemoveBlockingWebRequestListenerHaiku(*m_frameIdentifier, m_type, removedBlockingCount), extensionContext().identifier());
#endif
}

bool WebExtensionAPIWebRequestEvent::hasListener(RefPtr<WebExtensionCallbackHandler> listener)
{
    return m_listeners.containsIf([&](auto& entry) {
        return entry.callback->callbackFunction() == listener->callbackFunction();
    });
}

void WebExtensionAPIWebRequestEvent::removeAllListeners()
{
    if (m_listeners.isEmpty())
        return;

    WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(*m_frameIdentifier, m_type, contentWorldType(), m_listeners.size()), extensionContext().identifier());
#if PLATFORM(HAIKU)
    uint64_t blockingCount = 0;
    for (auto& entry : m_listeners) {
        if (isBlockingListenerHaiku(entry.extraInfo))
            ++blockingCount;
    }
    if (blockingCount)
        WebProcess::singleton().send(Messages::WebExtensionContext::RemoveBlockingWebRequestListenerHaiku(*m_frameIdentifier, m_type, blockingCount), extensionContext().identifier());
#endif

    m_listeners.clear();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
