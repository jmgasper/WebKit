/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
#include "WebExtensionAPIWebNavigation.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionNavigationParameters.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"

namespace WebKit {

void WebExtensionAPIWebNavigation::getAllFrames(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    auto identifier = parseWebExtensionNavigationTab(details.get());
    if (!identifier) {
        outExceptionString = identifier.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WebNavigationGetAllFrames(*identifier), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionFrameParameters>, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        auto frames = JSON::Array::create();
        for (auto& frame : *result)
            frames->pushObject(webExtensionNavigationFrameJSON(frame));
        callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString(frames->toJSONString()).get()));
    }, extensionContext().identifier());
}

void WebExtensionAPIWebNavigation::getFrame(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    auto tab = parseWebExtensionNavigationTab(details.get());
    if (!tab) {
        outExceptionString = tab.error();
        return;
    }
    auto frame = parseWebExtensionNavigationFrame(details.get());
    if (!frame) {
        outExceptionString = frame.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WebNavigationGetFrame(*tab, *frame), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionFrameParameters>, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        auto context = callback->globalContext();
        callback->call(*result ? JSValueMakeFromJSONString(context, toJSString(webExtensionNavigationFrameJSON(**result)->toJSONString()).get()) : JSValueMakeNull(context));
    }, extensionContext().identifier());
}

WebExtensionAPIWebNavigationEvent& WebExtensionAPIWebNavigation::onBeforeNavigate()
{
    // Documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webNavigation/onBeforeNavigate

    if (!m_onBeforeNavigateEvent)
        m_onBeforeNavigateEvent = WebExtensionAPIWebNavigationEvent::create(*this, WebExtensionEventListenerType::WebNavigationOnBeforeNavigate);

    return *m_onBeforeNavigateEvent;
}

WebExtensionAPIWebNavigationEvent& WebExtensionAPIWebNavigation::onCommitted()
{
    // Documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webNavigation/onCommitted

    if (!m_onCommittedEvent)
        m_onCommittedEvent = WebExtensionAPIWebNavigationEvent::create(*this, WebExtensionEventListenerType::WebNavigationOnCommitted);

    return *m_onCommittedEvent;
}

WebExtensionAPIWebNavigationEvent& WebExtensionAPIWebNavigation::onDOMContentLoaded()
{
    // Documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webNavigation/onDOMContentLoaded

    if (!m_onDOMContentLoadedEvent)
        m_onDOMContentLoadedEvent = WebExtensionAPIWebNavigationEvent::create(*this, WebExtensionEventListenerType::WebNavigationOnDOMContentLoaded);

    return *m_onDOMContentLoadedEvent;
}

WebExtensionAPIWebNavigationEvent& WebExtensionAPIWebNavigation::onCompleted()
{
    // Documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webNavigation/onCompleted

    if (!m_onCompletedEvent)
        m_onCompletedEvent = WebExtensionAPIWebNavigationEvent::create(*this, WebExtensionEventListenerType::WebNavigationOnCompleted);

    return *m_onCompletedEvent;
}

WebExtensionAPIWebNavigationEvent& WebExtensionAPIWebNavigation::onErrorOccurred()
{
    // Documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webNavigation/onErrorOccurred

    if (!m_onErrorOccurredEvent)
        m_onErrorOccurredEvent = WebExtensionAPIWebNavigationEvent::create(*this, WebExtensionEventListenerType::WebNavigationOnErrorOccurred);

    return *m_onErrorOccurredEvent;
}

void WebExtensionContextProxy::dispatchWebNavigationEvent(WebExtensionEventListenerType type, WebExtensionTabIdentifier tabID, const WebExtensionFrameParameters& frameParameters, WallTime timestamp)
{
    auto details = webExtensionNavigationEventJSON(tabID, frameParameters, timestamp);
    if (!details)
        return;
    auto json = details->toJSONString();
    enumerateNamespaceObjects([&](auto& namespaceObject) {
        auto& navigation = namespaceObject.webNavigation();
        WebExtensionAPIWebNavigationEvent* event = nullptr;
        switch (type) {
        case WebExtensionEventListenerType::WebNavigationOnBeforeNavigate: event = &navigation.onBeforeNavigate(); break;
        case WebExtensionEventListenerType::WebNavigationOnCommitted: event = &navigation.onCommitted(); break;
        case WebExtensionEventListenerType::WebNavigationOnDOMContentLoaded: event = &navigation.onDOMContentLoaded(); break;
        case WebExtensionEventListenerType::WebNavigationOnCompleted: event = &navigation.onCompleted(); break;
        case WebExtensionEventListenerType::WebNavigationOnErrorOccurred: event = &navigation.onErrorOccurred(); break;
        default: ASSERT_NOT_REACHED(); return;
        }
        event->invokeListenersWithJSONArgument(json, *frameParameters.url);
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
