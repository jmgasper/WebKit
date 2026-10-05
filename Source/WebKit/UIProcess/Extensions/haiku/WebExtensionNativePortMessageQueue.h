/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionConstants.h"
#include <wtf/Deque.h>
#include <wtf/JSONValues.h>
#include <wtf/MainThread.h>
#include <wtf/RefCounted.h>
#include <wtf/SharedTask.h>

namespace WebKit {

// The native callback boundary carries validated JSON text, including fragments.
// Confined to the WebKit main thread; callbacks can replace themselves or close it.
class WebExtensionNativePortMessageQueue : public RefCounted<WebExtensionNativePortMessageQueue> {
public:
    using MessageHandler = SharedTask<void(const String&)>;
    enum class Result : uint8_t { Accepted, Closed, InvalidMessage, QueueFull };

    static Ref<WebExtensionNativePortMessageQueue> create() { return adoptRef(*new WebExtensionNativePortMessageQueue); }
    bool isClosed() const { return m_closed; }

    static bool isValidMessage(const String& message)
    {
        return message.length() <= webExtensionMaxMessageLength && !!JSON::Value::parseJSON(message);
    }

    void setMessageHandler(RefPtr<MessageHandler>&& handler)
    {
        ASSERT(isMainThread());
        Ref protectedThis { *this };
        if (m_closed)
            return;
        m_handler = WTF::move(handler);
        drain();
    }

    Result receive(Vector<String>&& messages)
    {
        ASSERT(isMainThread());
        Ref protectedThis { *this };
        if (m_closed)
            return Result::Closed;
        constexpr size_t maximumQueuedMessages = 1024;
        if (messages.size() > maximumQueuedMessages - m_messages.size())
            return Result::QueueFull;
        size_t length = m_queuedLength;
        for (auto& message : messages) {
            if (!isValidMessage(message))
                return Result::InvalidMessage;
            if (message.length() > webExtensionMaxMessageLength - length)
                return Result::QueueFull;
            length += message.length();
        }
        m_queuedLength = length;
        for (auto& message : messages)
            m_messages.append(WTF::move(message));
        drain();
        return Result::Accepted;
    }

    void close()
    {
        ASSERT(isMainThread());
        m_closed = true;
        m_messages.clear();
        m_queuedLength = 0;
        m_handler = nullptr;
    }

private:
    WebExtensionNativePortMessageQueue() = default;

    void drain()
    {
        if (m_delivering)
            return;
        m_delivering = true;
        while (!m_closed && m_handler && !m_messages.isEmpty()) {
            Ref handler { *m_handler };
            auto message = m_messages.takeFirst();
            m_queuedLength -= message.length();
            handler->run(message);
        }
        m_delivering = false;
    }

    RefPtr<MessageHandler> m_handler;
    Deque<String> m_messages;
    size_t m_queuedLength { 0 };
    bool m_closed { false };
    bool m_delivering { false };
};

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
