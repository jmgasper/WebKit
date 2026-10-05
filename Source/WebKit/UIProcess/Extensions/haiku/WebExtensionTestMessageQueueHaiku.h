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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionEventListenerType.h"
#include <wtf/Deque.h>

namespace WebKit {

// Query the actual weak frame registry. Separate listener totals can remain
// positive after multiple registrations are removed or a frame disappears.
template<typename EventListenerMap>
bool hasWebExtensionTestEventListenersHaiku(WebExtensionEventListenerType type, const EventListenerMap& listeners)
{
    switch (type) {
    case WebExtensionEventListenerType::TestOnMessage:
    case WebExtensionEventListenerType::TestOnTestStarted:
    case WebExtensionEventListenerType::TestOnTestFinished:
        break;
    default:
        return false;
    }
    for (auto& entry : listeners) {
        if (entry.key.first == type && !entry.value.isEmptyIgnoringNullReferences())
            return true;
    }
    return false;
}

// Keep queued messages when delivery is unavailable. Limit each drain to its
// initial queue size so reentrant enqueues cannot prolong it indefinitely.
template<typename Message, typename CanSend, typename Send>
void drainWebExtensionTestMessagesHaiku(Deque<Message>& queue, CanSend&& canSend, Send&& send)
{
    for (size_t remaining = queue.size(); remaining && !queue.isEmpty() && canSend(); --remaining)
        send(queue.takeFirst());
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
