/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
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

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionTestMessageQueueHaiku.h"

namespace WebKit {

void WebExtensionContext::sendTestMessage(const String& message, Ref<JSON::Value> argument)
{
    ASSERT(isLoaded() && inTestingMode());
    if (!isLoaded() || !inTestingMode())
        return;

    constexpr auto eventType = WebExtensionEventListenerType::TestOnMessage;

    if (!hasTestEventListeners(eventType)) {
        addTestMessageToQueue(message, WTF::move(argument), eventType);
        return;
    }

    String argumentJSON = argument->toJSONString();

    sendToProcesses(processes(eventType, WebExtensionContentWorldType::WebPage), Messages::WebExtensionContextProxy::DispatchTestMessageEvent(message, argumentJSON, WebExtensionContentWorldType::WebPage));

    sendToContentScriptProcessesForEvent(eventType, Messages::WebExtensionContextProxy::DispatchTestMessageEvent(message, argumentJSON, WebExtensionContentWorldType::ContentScript));

    wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }] {
        sendToProcessesForEvent(eventType, Messages::WebExtensionContextProxy::DispatchTestMessageEvent(message, argumentJSON, WebExtensionContentWorldType::Main));
    });
}

void WebExtensionContext::sendTestStarted(Ref<JSON::Value> argument)
{
    ASSERT(isLoaded() && inTestingMode());
    if (!isLoaded() || !inTestingMode())
        return;

    constexpr auto type = WebExtensionEventListenerType::TestOnTestStarted;

    if (!hasTestEventListeners(type)) {
        addTestMessageToQueue(nullString(), WTF::move(argument), type);
        return;
    }

    String argumentJSON = argument->toJSONString();

    sendToProcesses(processes(type, WebExtensionContentWorldType::WebPage), Messages::WebExtensionContextProxy::DispatchTestStartedEvent(argumentJSON, WebExtensionContentWorldType::WebPage));

    sendToContentScriptProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTestStartedEvent(argumentJSON, WebExtensionContentWorldType::ContentScript));

    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [=, this, protectedThis = Ref { *this }] {
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTestStartedEvent(argumentJSON, WebExtensionContentWorldType::Main));
    });
}

void WebExtensionContext::sendTestFinished(Ref<JSON::Value> argument)
{
    ASSERT(isLoaded() && inTestingMode());
    if (!isLoaded() || !inTestingMode())
        return;

    constexpr auto type = WebExtensionEventListenerType::TestOnTestFinished;

    if (!hasTestEventListeners(type)) {
        addTestMessageToQueue(nullString(), WTF::move(argument), type);
        return;
    }

    String argumentJSON = argument->toJSONString();

    sendToProcesses(processes(type, WebExtensionContentWorldType::WebPage), Messages::WebExtensionContextProxy::DispatchTestFinishedEvent(argumentJSON, WebExtensionContentWorldType::WebPage));

    sendToContentScriptProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTestFinishedEvent(argumentJSON, WebExtensionContentWorldType::ContentScript));

    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [=, this, protectedThis = Ref { *this }] {
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTestFinishedEvent(argumentJSON, WebExtensionContentWorldType::Main));
    });
}

void WebExtensionContext::addTestMessageToQueue(const String& message, Ref<JSON::Value> argument, WebExtensionEventListenerType type)
{
    switch (type) {
    case WebExtensionEventListenerType::TestOnMessage:
        m_testMessageQueue.append({ message, WTF::move(argument) });
        break;

    case WebExtensionEventListenerType::TestOnTestStarted:
        m_testStartedQueue.append({ message, WTF::move(argument) });
        break;

    case WebExtensionEventListenerType::TestOnTestFinished:
        m_testFinishedQueue.append({ message, WTF::move(argument) });
        break;

    default:
        ASSERT_NOT_REACHED();
        return;
    }
}

void WebExtensionContext::sendQueuedTestMessagesIfNeeded(WebExtensionEventListenerType type)
{
    auto canSend = [&] { return isLoaded() && inTestingMode() && hasTestEventListeners(type); };
    switch (type) {
    case WebExtensionEventListenerType::TestOnMessage:
        drainWebExtensionTestMessagesHaiku(m_testMessageQueue, canSend, [&](auto message) {
            sendTestMessage(message.message, WTF::move(message.argument));
        });
        break;
    case WebExtensionEventListenerType::TestOnTestStarted:
        drainWebExtensionTestMessagesHaiku(m_testStartedQueue, canSend, [&](auto message) {
            sendTestStarted(WTF::move(message.argument));
        });
        break;
    case WebExtensionEventListenerType::TestOnTestFinished:
        drainWebExtensionTestMessagesHaiku(m_testFinishedQueue, canSend, [&](auto message) {
            sendTestFinished(WTF::move(message.argument));
        });
        break;
    default:
        ASSERT_NOT_REACHED();
        return;
    }
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
