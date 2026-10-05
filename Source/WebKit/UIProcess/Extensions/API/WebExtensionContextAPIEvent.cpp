/*
 * Copyright (C) 2022-2024 Apple Inc. All rights reserved.
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
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "WebExtensionController.h"
#include "WebExtensionAction.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionFrameParameters.h"
#include "WebExtensionPermission.h"
#include "WebExtensionTab.h"
#include "WebPageProxy.h"
#include "WebUserContentControllerProxy.h"
#include "WebFrameProxy.h"
#include "WebProcessProxy.h"
#include <wtf/EnumTraits.h>

namespace WebKit {

static inline bool NODELETE isTestEventListener(WebExtensionEventListenerType type)
{
    return type == WebExtensionEventListenerType::TestOnMessage
        || type == WebExtensionEventListenerType::TestOnTestStarted
        || type == WebExtensionEventListenerType::TestOnTestFinished;
}

void WebExtensionContext::addListener(WebCore::FrameIdentifier frameIdentifier, WebExtensionEventListenerType listenerType, WebExtensionContentWorldType contentWorldType)
{
    RefPtr frame = WebFrameProxy::webFrame(frameIdentifier);
    if (!frame)
        return;

    RELEASE_LOG_DEBUG(Extensions, "Registered event listener for type %hhu in %" PUBLIC_LOG_STRING " world", std::to_underlying(listenerType), toDebugString(contentWorldType).utf8().legacyCStringPointer());

    if (!protect(extension())->backgroundContentIsPersistent() && isBackgroundPage(frameIdentifier))
        m_backgroundContentEventListeners.add(listenerType);

    auto result = m_eventListenerFrames.add({ listenerType, contentWorldType }, WeakFrameCountedSet { });
    result.iterator->value.add(*frame);

    if (!isTestEventListener(listenerType))
        return;

#if !PLATFORM(HAIKU)
    switch (listenerType) {
    case WebExtensionEventListenerType::TestOnMessage:
        m_testMessageListenersCount++;
        break;

    case WebExtensionEventListenerType::TestOnTestStarted:
        m_testStartedListenersCount++;
        break;

    case WebExtensionEventListenerType::TestOnTestFinished:
        m_testFinishedListenersCount++;
        break;

    default:
        ASSERT_NOT_REACHED();
        break;
    }
#endif

    sendQueuedTestMessagesIfNeeded(listenerType);
}

void WebExtensionContext::removeListener(WebCore::FrameIdentifier frameIdentifier, WebExtensionEventListenerType listenerType, WebExtensionContentWorldType contentWorldType, uint64_t removedCount)
{
    ASSERT(removedCount);

    RefPtr frame = WebFrameProxy::webFrame(frameIdentifier);
    if (!frame)
        return;

    RELEASE_LOG_DEBUG(Extensions, "Unregistered %llu event listener(s) for type %hhu in %" PUBLIC_LOG_STRING " world", removedCount, std::to_underlying(listenerType), toDebugString(contentWorldType).utf8().legacyCStringPointer());

    if (!protect(extension())->backgroundContentIsPersistent() && isBackgroundPage(frameIdentifier)) {
        for (size_t i = 0; i < removedCount; ++i)
            m_backgroundContentEventListeners.remove(listenerType);
    }

    auto iterator = m_eventListenerFrames.find({ listenerType, contentWorldType });
    if (iterator == m_eventListenerFrames.end())
        return;

    for (size_t i = 0; i < removedCount; ++i)
        iterator->value.remove(*frame);

    if (!iterator->value.isEmptyIgnoringNullReferences())
        return;

    m_eventListenerFrames.remove(iterator);

#if !PLATFORM(HAIKU)
    if (!isTestEventListener(listenerType) || !hasTestEventListeners(listenerType))
        return;

    switch (listenerType) {
    case WebExtensionEventListenerType::TestOnMessage:
        m_testMessageListenersCount--;
        break;

    case WebExtensionEventListenerType::TestOnTestStarted:
        m_testStartedListenersCount--;
        break;

    case WebExtensionEventListenerType::TestOnTestFinished:
        m_testFinishedListenersCount--;
        break;

    default:
        ASSERT_NOT_REACHED();
        break;
    }
#endif
}

void WebExtensionContext::didStartProvisionalLoadForFrame(WebPageProxyIdentifier pageID, const WebExtensionFrameParameters& frameParameters, WallTime timestamp)
{
    ASSERT(frameParameters.url);

    RefPtr tab = getTab(pageID);
    auto& frameURL = *frameParameters.url;
#if PLATFORM(HAIKU)
    if (tab && frameParameters.frameIdentifier && isMainFrame(*frameParameters.frameIdentifier))
        closeNativeActionPopupForTab(*tab);
#endif

    // Dispatch webNavigation events.
    if (tab && hasPermission(WebExtensionPermission::webNavigation(), tab.get()) && hasPermission(frameURL, tab.get())) {
        constexpr auto eventType = WebExtensionEventListenerType::WebNavigationOnBeforeNavigate;
        wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }] {
            sendToProcessesForEvent(eventType, Messages::WebExtensionContextProxy::DispatchWebNavigationEvent(eventType, tab->identifier(), frameParameters, timestamp));
        });
    }
}

void WebExtensionContext::didCommitLoadForFrame(WebPageProxyIdentifier pageID, const WebExtensionFrameParameters& frameParameters, WallTime timestamp)
{
    ASSERT(frameParameters.url);
    ASSERT(frameParameters.frameIdentifier);

    RefPtr page = WebProcessProxy::webPage(pageID);
    if (!page)
        return;

    auto& frameURL = *frameParameters.url;
    auto& frameID = *frameParameters.frameIdentifier;

    RefPtr tab = getTab(pageID);

    if (tab && isMainFrame(frameID)) {
        // Clear tab action customizations.
        if (RefPtr tabAction = m_actionTabMap.get(*tab))
            tabAction->clearCustomizations();

#if PLATFORM(HAIKU)
        closeNativeActionPopupForTab(*tab);
        // Same-origin navigations retain the tab grant. A new origin (including
        // a changed port or an opaque document) revokes it before event delivery.
        tab->nativeDidCommitNavigation();
#else
        // Clear activeTab permissions and user gesture if the site changed.
        auto temporaryPattern = tab->temporaryPermissionMatchPattern();
        if (temporaryPattern && !temporaryPattern->matchesURL(frameURL))
            clearUserGesture(*tab);
#endif

        // Clear injected styles tied to this specific page.
        // FIXME: <https://webkit.org/b/262491> There is currently no way to inject CSS in specific frames based on ID's.
        Ref userContentController = page.get()->userContentController();
        m_dynamicallyInjectedUserStyleSheets.removeAllMatching([&](auto& styleSheet) {
            if (styleSheet->page() != page.get())
                return false;

            userContentController->removeUserStyleSheet(styleSheet);
            return true;
        });
    }

    // Dispatch webNavigation events.
    if (tab && hasPermission(WebExtensionPermission::webNavigation(), tab.get()) && hasPermission(frameURL, tab.get())) {
        constexpr auto committedEventType = WebExtensionEventListenerType::WebNavigationOnCommitted;
        constexpr auto contentEventType = WebExtensionEventListenerType::WebNavigationOnDOMContentLoaded;

        wakeUpBackgroundContentIfNecessaryToFireEvents({ committedEventType, contentEventType }, [=, this, protectedThis = Ref { *this }] {
            sendToProcessesForEvent(committedEventType, Messages::WebExtensionContextProxy::DispatchWebNavigationEvent(committedEventType, tab->identifier(), frameParameters, timestamp));
            sendToProcessesForEvent(contentEventType, Messages::WebExtensionContextProxy::DispatchWebNavigationEvent(contentEventType, tab->identifier(), frameParameters, timestamp));
        });
    }
}

void WebExtensionContext::didFinishLoadForFrame(WebPageProxyIdentifier pageID, const WebExtensionFrameParameters& frameParameters, WallTime timestamp)
{
    ASSERT(frameParameters.url);

    RefPtr tab = getTab(pageID);
    auto& frameURL = *frameParameters.url;

    // Dispatch webNavigation events.
    if (tab && hasPermission(WebExtensionPermission::webNavigation(), tab.get()) && hasPermission(frameURL, tab.get())) {
        constexpr auto eventType = WebExtensionEventListenerType::WebNavigationOnCompleted;
        wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }] {
            sendToProcessesForEvent(eventType, Messages::WebExtensionContextProxy::DispatchWebNavigationEvent(eventType, tab->identifier(), frameParameters, timestamp));
        });
    }
}

void WebExtensionContext::didFailLoadForFrame(WebPageProxyIdentifier pageID, const WebExtensionFrameParameters& frameParameters, WallTime timestamp)
{
    ASSERT(frameParameters.url);

    RefPtr tab = getTab(pageID);
    auto& frameURL = *frameParameters.url;

    // Dispatch webNavigation events.
    if (tab && hasPermission(WebExtensionPermission::webNavigation(), tab.get()) && hasPermission(frameURL, tab.get())) {
        constexpr auto eventType = WebExtensionEventListenerType::WebNavigationOnErrorOccurred;
        wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }] {
            sendToProcessesForEvent(eventType, Messages::WebExtensionContextProxy::DispatchWebNavigationEvent(eventType, tab->identifier(), frameParameters, timestamp));
        });
    }
}

#if !PLATFORM(HAIKU)
void WebExtensionContext::cookiesDidChange(API::HTTPCookieStore&)
{
    // FIXME: <https://webkit.org/b/267514> Add support for changeInfo.

    fireCookiesChangedEventIfNeeded();
}

void WebExtensionContext::fireCookiesChangedEventIfNeeded()
{
    constexpr auto type = WebExtensionEventListenerType::CookiesOnChanged;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [=, this, protectedThis = Ref { *this }] {
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchCookiesChangedEvent());
    });
}

#endif // !PLATFORM(HAIKU)

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
