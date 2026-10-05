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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionAPIWebNavigation.h"
#include "WebExtensionAPIObject.h"
#include "WebExtensionAPIWebNavigationEvent.h"

namespace WebKit {

class WebPage;

class WebExtensionAPIWebNavigation : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIWebNavigation, webNavigation, webNavigation);

public:
    WebExtensionAPIWebNavigationEvent& onBeforeNavigate();
    WebExtensionAPIWebNavigationEvent& onCommitted();
    WebExtensionAPIWebNavigationEvent& onDOMContentLoaded();
    WebExtensionAPIWebNavigationEvent& onCompleted();
    WebExtensionAPIWebNavigationEvent& onErrorOccurred();
#if PLATFORM(HAIKU)
    // Registered so extensions can attach listeners; these are not dispatched yet.
    WebExtensionAPIWebNavigationEvent& onCreatedNavigationTarget() { return additionalEvent(m_onCreatedNavigationTargetEvent, WebExtensionEventListenerType::WebNavigationOnCreatedNavigationTarget); }
    WebExtensionAPIWebNavigationEvent& onHistoryStateUpdated() { return additionalEvent(m_onHistoryStateUpdatedEvent, WebExtensionEventListenerType::WebNavigationOnHistoryStateUpdated); }
    WebExtensionAPIWebNavigationEvent& onReferenceFragmentUpdated() { return additionalEvent(m_onReferenceFragmentUpdatedEvent, WebExtensionEventListenerType::WebNavigationOnReferenceFragmentUpdated); }
    WebExtensionAPIWebNavigationEvent& onTabReplaced() { return additionalEvent(m_onTabReplacedEvent, WebExtensionEventListenerType::WebNavigationOnTabReplaced); }
#endif

    void getAllFrames(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void getFrame(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);

private:
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onBeforeNavigateEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onCommittedEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onDOMContentLoadedEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onCompletedEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onErrorOccurredEvent;
#if PLATFORM(HAIKU)
    WebExtensionAPIWebNavigationEvent& additionalEvent(RefPtr<WebExtensionAPIWebNavigationEvent>& event, WebExtensionEventListenerType type)
    {
        if (!event)
            event = WebExtensionAPIWebNavigationEvent::create(*this, type);
        return *event;
    }
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onCreatedNavigationTargetEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onHistoryStateUpdatedEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onReferenceFragmentUpdatedEvent;
    RefPtr<WebExtensionAPIWebNavigationEvent> m_onTabReplacedEvent;
#endif
};

} // namespace WebKit

SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIWebNavigation, webNavigation);

#endif // ENABLE(WK_WEB_EXTENSIONS)
