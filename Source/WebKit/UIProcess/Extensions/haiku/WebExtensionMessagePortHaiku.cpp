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

#include "config.h"
#include "WebExtensionMessagePort.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContext.h"
#include <wtf/RunLoop.h>

namespace WebKit {

static std::optional<String> disconnectMessage(const WebExtensionMessagePort::Error& error)
{
    if (!error)
        return std::nullopt;
    if (error->second && !error->second->isEmpty())
        return *error->second;
    switch (error->first) {
    case WebExtensionMessagePort::ErrorType::Unknown:
        return "An unknown native message-port error occurred."_s;
    case WebExtensionMessagePort::ErrorType::NotConnected:
        return "The native message port is not connected."_s;
    case WebExtensionMessagePort::ErrorType::MessageInvalid:
        return "The native message is not valid JSON or exceeds the message limit."_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

WebExtensionMessagePort::WebExtensionMessagePort(WebExtensionContext& context, String applicationIdentifier, WebExtensionPortChannelIdentifier channelIdentifier)
    : m_messageQueue(WebExtensionNativePortMessageQueue::create())
    , m_loadIdentifier(context.isLoaded() ? std::make_optional(context.privilegedIdentifier()) : std::nullopt)
    , m_extensionContext(context)
    , m_applicationIdentifier(WTF::move(applicationIdentifier))
    , m_channelIdentifier(channelIdentifier)
{
    ASSERT(isMainThread());
}

WebExtensionMessagePort::~WebExtensionMessagePort()
{
    // Registered ports are strongly owned by the context and explicitly detached
    // before its map is cleared. An unregistered port has no membership to remove.
    m_messageQueue->close();
}

bool WebExtensionMessagePort::operator==(const WebExtensionMessagePort& other) const
{
    return this == &other || (!isDisconnected() && !other.isDisconnected()
        && m_extensionContext == other.m_extensionContext && m_loadIdentifier == other.m_loadIdentifier
        && m_applicationIdentifier == other.m_applicationIdentifier && m_channelIdentifier == other.m_channelIdentifier);
}

WebExtensionContext* WebExtensionMessagePort::extensionContext() const
{
    return m_extensionContext.get();
}

bool WebExtensionMessagePort::isDisconnected() const
{
    RefPtr context = m_extensionContext.get();
    return m_messageQueue->isClosed() || !context || !context->isLoaded() || !m_loadIdentifier
        || context->privilegedIdentifier() != *m_loadIdentifier;
}

void WebExtensionMessagePort::setMessageHandler(RefPtr<MessageHandler>&& handler)
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    if (!isDisconnected())
        m_messageQueue->setMessageHandler(WTF::move(handler));
}

void WebExtensionMessagePort::setDisconnectHandler(RefPtr<DisconnectHandler>&& handler)
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    if (!isDisconnected())
        m_disconnectHandler = WTF::move(handler);
}

void WebExtensionMessagePort::remove(Error error)
{
    ASSERT(isMainThread());
    RefPtr context = m_extensionContext.get();
    bool currentLoad = !isDisconnected();
    // Detach before dropping the map's reference or invoking any host callback.
    m_extensionContext = nullptr;
    m_messageQueue->close();
    m_disconnectHandler = nullptr;
    if (!currentLoad || !context || context->m_nativePortMap.get(m_channelIdentifier) != this)
        return;
    context->removeNativePort(*this);
    context->firePortDisconnectEventIfNeeded(WebExtensionContentWorldType::Native, WebExtensionContentWorldType::Main, m_channelIdentifier, disconnectMessage(error));
}

void WebExtensionMessagePort::disconnect(Error error)
{
    Ref protectedThis { *this };
    remove(WTF::move(error));
}

void WebExtensionMessagePort::reportDisconnection(Error error)
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    if (m_messageQueue->isClosed())
        return;
    RefPtr handler = m_disconnectHandler;
    remove(error);
    if (handler)
        handler->run(WTF::move(error));
}

void WebExtensionMessagePort::disconnectForContextUnload()
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    if (m_messageQueue->isClosed())
        return;
    RefPtr handler = WTF::move(m_disconnectHandler);
    m_extensionContext = nullptr;
    m_messageQueue->close();
    // Context unload still has maps and background state to clear. A callback
    // may reload that context, so run it only after the unload stack unwinds.
    if (handler) {
        RunLoop::mainSingleton().dispatch([handler = WTF::move(handler)] {
            handler->run({ { ErrorType::NotConnected, "The extension context was unloaded."_s } });
        });
    }
}

void WebExtensionMessagePort::sendMessage(const String& messageJSON, CompletionHandler<void(Error)>&& completionHandler)
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    Error error;
    RefPtr context = m_extensionContext.get();
    if (isDisconnected() || !context || context->m_nativePortMap.get(m_channelIdentifier) != this)
        error = { { ErrorType::NotConnected, std::nullopt } };
    else if (!WebExtensionNativePortMessageQueue::isValidMessage(messageJSON))
        error = { { ErrorType::MessageInvalid, std::nullopt } };
    else
        context->portPostMessage(WebExtensionContentWorldType::Native, WebExtensionContentWorldType::Main, std::nullopt, m_channelIdentifier, messageJSON, false);
    if (completionHandler)
        completionHandler(WTF::move(error));
}

void WebExtensionMessagePort::receiveMessages(Vector<String>&& messages)
{
    ASSERT(isMainThread());
    Ref protectedThis { *this };
    if (isDisconnected())
        return;
    switch (m_messageQueue->receive(WTF::move(messages))) {
    case WebExtensionNativePortMessageQueue::Result::Accepted:
    case WebExtensionNativePortMessageQueue::Result::Closed:
        return;
    case WebExtensionNativePortMessageQueue::Result::InvalidMessage:
        reportDisconnection({ { ErrorType::MessageInvalid, std::nullopt } });
        return;
    case WebExtensionNativePortMessageQueue::Result::QueueFull:
        reportDisconnection({ { ErrorType::MessageInvalid, "The native message queue limit was exceeded."_s } });
        return;
    }
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
