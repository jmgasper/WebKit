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
#include "WebExtensionAPIWebNavigationEvent.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebProcess.h"

namespace WebKit {

void WebExtensionAPIWebNavigationEvent::invokeListenersWithJSONArgument(const String& argument, const URL& targetURL)
{
    // Callbacks can change listener registration. Keep the original snapshot alive.
    auto listenersCopy = m_listeners;
    for (auto& listener : listenersCopy) {
        if (listener.second && !listener.second->matches(targetURL))
            continue;
        Ref callback = *listener.first;
        auto context = callback->globalContext();
        callback->call(JSValueMakeFromJSONString(context, toJSString(argument).get()));
    }
}

void WebExtensionAPIWebNavigationEvent::addListener(WebCore::FrameIdentifier frameIdentifier, RefPtr<WebExtensionCallbackHandler> listener, RefPtr<JSON::Value> filter, String& outExceptionString)
{
    std::optional<WebExtensionNavigationURLFilter> parsedFilter;
    if (filter && !filter->isNull()) {
        auto parsed = WebExtensionNavigationURLFilter::parse(*filter);
        if (!parsed) {
            outExceptionString = parsed.error();
            return;
        }
        parsedFilter = WTF::move(*parsed);
    }
    m_frameIdentifier = frameIdentifier;
    m_listeners.append({ WTF::move(listener), WTF::move(parsedFilter) });
    WebProcess::singleton().send(Messages::WebExtensionContext::AddListener(*m_frameIdentifier, m_type, contentWorldType()), extensionContext().identifier());
}

void WebExtensionAPIWebNavigationEvent::removeListener(WebCore::FrameIdentifier frameIdentifier, RefPtr<WebExtensionCallbackHandler> listener)
{
    auto removedCount = m_listeners.removeAllMatching([&](auto& entry) {
        return entry.first->callbackFunction() == listener->callbackFunction();
    });

    if (!removedCount)
        return;

    ASSERT(frameIdentifier == m_frameIdentifier);

    WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(*m_frameIdentifier, m_type, contentWorldType(), removedCount), extensionContext().identifier());
}

bool WebExtensionAPIWebNavigationEvent::hasListener(RefPtr<WebExtensionCallbackHandler> listener)
{
    return m_listeners.containsIf([&](auto& entry) {
        return entry.first->callbackFunction() == listener->callbackFunction();
    });
}

void WebExtensionAPIWebNavigationEvent::removeAllListeners()
{
    if (m_listeners.isEmpty())
        return;

    WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(*m_frameIdentifier, m_type, contentWorldType(), m_listeners.size()), extensionContext().identifier());

    m_listeners.clear();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
