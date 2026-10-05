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

#if ENABLE(WK_WEB_EXTENSIONS)

#include "APIObject.h"
#include "WebExtensionPortChannelIdentifier.h"
#if PLATFORM(HAIKU)
#include "WebExtensionContextIdentifier.h"
#include "haiku/WebExtensionNativePortMessageQueue.h"
#endif
#include <wtf/CanMakeWeakPtr.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>
#include <wtf/WeakPtr.h>

OBJC_CLASS NSError;
OBJC_CLASS WKWebExtensionMessagePort;

namespace WebKit {

class WebExtensionContext;

class WebExtensionMessagePort : public API::ObjectImpl<API::Object::Type::WebExtensionMessagePort>, public CanMakeWeakPtr<WebExtensionMessagePort> {
    WTF_MAKE_NONCOPYABLE(WebExtensionMessagePort);

public:
    template<typename... Args>
    static Ref<WebExtensionMessagePort> create(Args&&... args)
    {
        return adoptRef(*new WebExtensionMessagePort(std::forward<Args>(args)...));
    }

    explicit WebExtensionMessagePort(WebExtensionContext&, String applicationIdentifier, WebExtensionPortChannelIdentifier);

    ~WebExtensionMessagePort();

    enum class ErrorType : uint8_t {
        Unknown = 1,
        NotConnected,
        MessageInvalid,
    };

    using Error = std::optional<std::pair<ErrorType, std::optional<String>>>;

    bool NODELETE operator==(const WebExtensionMessagePort&) const;

    const String& applicationIdentifier() const LIFETIME_BOUND { return m_applicationIdentifier; }
    WebExtensionPortChannelIdentifier channelIdentifier() const { return m_channelIdentifier; }
    WebExtensionContext* NODELETE extensionContext() const;

    void disconnect(Error = std::nullopt);
    void reportDisconnection(Error);
    bool NODELETE isDisconnected() const;

#if PLATFORM(HAIKU)
    using MessageHandler = WebExtensionNativePortMessageQueue::MessageHandler;
    using DisconnectHandler = SharedTask<void(Error)>;
    void setMessageHandler(RefPtr<MessageHandler>&&);
    void setDisconnectHandler(RefPtr<DisconnectHandler>&&);
    void sendMessage(const String& messageJSON, CompletionHandler<void(Error)>&& = { });
    void receiveMessages(Vector<String>&&);
    void disconnectForContextUnload();
#endif

#if PLATFORM(COCOA)
    void sendMessage(id message, CompletionHandler<void(Error)>&& = { });
    void receiveMessage(id message, Error);
#endif

#ifdef __OBJC__
    WKWebExtensionMessagePort *wrapper() const { return (WKWebExtensionMessagePort *)API::ObjectImpl<API::Object::Type::WebExtensionMessagePort>::wrapper(); }
#endif

private:
#if PLATFORM(HAIKU)
    void remove(Error);
    Ref<WebExtensionNativePortMessageQueue> m_messageQueue;
    RefPtr<DisconnectHandler> m_disconnectHandler;
    std::optional<WebExtensionContextIdentifier> m_loadIdentifier;
#else
    void remove();
#endif

    WeakPtr<WebExtensionContext> m_extensionContext;
    String m_applicationIdentifier;
    WebExtensionPortChannelIdentifier m_channelIdentifier;
};

NSError *toAPI(WebExtensionMessagePort::Error);
WebExtensionMessagePort::Error toWebExtensionMessagePortError(NSError *);

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
