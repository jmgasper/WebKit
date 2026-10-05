/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "WebExtensionAPIPort.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Logging.h"
#include "MessageSenderInlines.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionAPIRuntime.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionControllerProxy.h"
#include "WebExtensionUtilities.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/UserGestureIndicator.h>

namespace WebKit {

using PortChannelPortMap = HashMap<WebExtensionPortChannelIdentifier, HashSet<WeakRef<WebExtensionAPIPort>>>;

static PortChannelPortMap& NODELETE webExtensionPorts()
{
    static MainRunLoopNeverDestroyed<PortChannelPortMap> ports;
    return ports;
}

WebExtensionAPIPort::PortSet WebExtensionAPIPort::get(WebExtensionPortChannelIdentifier identifier)
{
    PortSet result;

    auto entry = webExtensionPorts().find(identifier);
    if (entry == webExtensionPorts().end())
        return result;

    for (auto& port : entry->value)
        result.add(port.get());

    return result;
}

void WebExtensionAPIPort::add()
{
    ASSERT(!isQuarantined());

    auto addResult = webExtensionPorts().ensure(channelIdentifier(), [&] {
        return HashSet<WeakRef<WebExtensionAPIPort>> { };
    });

    addResult.iterator->value.add(*this);

    RELEASE_LOG_DEBUG(Extensions, "Added port for channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier().toUInt64(), toDebugString(contentWorldType()).utf8().data());
}

void WebExtensionAPIPort::remove()
{
    disconnect();

    if (isQuarantined())
        return;

    auto entry = webExtensionPorts().find(channelIdentifier());
    // Finalization can follow an explicit disconnect while other listeners
    // still own ports on this channel. Report removal only for this port.
    if (entry == webExtensionPorts().end() || !entry->value.contains(*this))
        return;

    WebProcess::singleton().send(Messages::WebExtensionContext::PortRemoved(contentWorldType(), targetContentWorldType(), *owningPageProxyIdentifier(), channelIdentifier()), extensionContext().identifier());

    entry->value.remove(*this);

    if (!entry->value.isEmpty())
        return;

    webExtensionPorts().remove(entry);
}

String WebExtensionAPIPort::name()
{
    return m_name;
}

JSValueRef WebExtensionAPIPort::sender(JSContextRef context)
{
    return m_senderParameters ? toWebAPI(context, m_senderParameters.value()) : JSValueMakeNull(context);
}

JSValueRef WebExtensionAPIPort::error(JSContextRef context)
{
    if (m_errorMessage) {
        protect(runtime())->markLastErrorAccessed();
        if (!m_error)
            setError(context, toJSError(context, *m_errorMessage));
    }
    return m_error ? m_error.get() : JSValueMakeNull(context);
}

void WebExtensionAPIPort::setError(JSContextRef context, JSValueRef error)
{
    m_error = Protected(JSContextGetGlobalContext(context), error);
}

void WebExtensionAPIPort::postMessage(WebFrame& frame, const String& message, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/Port#postmessage

    if (isDisconnected()) {
        outExceptionString = toErrorString(nullString(), nullString(), "the port is disconnected"_s);
        return;
    }

    if (message.length() > webExtensionMaxMessageLength) {
        outExceptionString = toErrorString(nullString(), "message"_s, "it exceeded the maximum allowed length"_s);
        return;
    }

    if (isQuarantined())
        return;

    RELEASE_LOG_DEBUG(Extensions, "Sent port message for channel %{public}llu from %" PUBLIC_LOG_STRING " world", channelIdentifier().toUInt64(), toDebugString(contentWorldType()).utf8().data());

    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().send(Messages::WebExtensionContext::PortPostMessage(contentWorldType(), targetContentWorldType(), owningPageProxyIdentifier(), channelIdentifier(), message, userGesture), extensionContext().identifier());
}

void WebExtensionAPIPort::disconnect()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/Port#disconnect

    fireDisconnectEventIfNeeded();
}

void WebExtensionAPIPort::fireMessageEventIfNeeded(const String& messageJSON, bool userGesture)
{
    if (isDisconnected() || isQuarantined() || !m_onMessage || m_onMessage->listeners().isEmpty())
        return;

    RELEASE_LOG_DEBUG(Extensions, "Fired port message event for channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier().toUInt64(), toDebugString(contentWorldType()).utf8().data());

    // Copy the listeners since call() can trigger a mutation of the listeners.
    auto listenersCopy = m_onMessage->listeners();

    for (RefPtr listener : listenersCopy) {
        auto globalContext = listener->globalContext();

        std::optional<WebCore::UserGestureIndicator> gestureIndicator;
        if (userGesture) {
            RefPtr frame = toWebFrame(globalContext);
            RefPtr coreFrame = frame ? frame->coreLocalFrame() : nullptr;
            gestureIndicator.emplace(WebCore::IsProcessingUserGesture::Yes, coreFrame ? coreFrame->document() : nullptr);
        }

        listener->call(
            deserializeJSONString(globalContext, messageJSON),
            toJS(globalContext, this)
        );
    }
}

void WebExtensionAPIPort::fireDisconnectEventIfNeeded(const std::optional<String>& errorMessage)
{
    if (isDisconnected())
        return;

    Ref protectedThis { *this };

    RELEASE_LOG_DEBUG(Extensions, "Port channel %{public}llu disconnected in %" PUBLIC_LOG_STRING " world", m_channelIdentifier ? m_channelIdentifier->toUInt64() : 0, toDebugString(contentWorldType()).utf8().data());

    m_errorMessage = errorMessage;
    m_disconnected = true;

    if (m_onMessage)
        m_onMessage->removeAllListeners();

    remove();

    if (!m_onDisconnect || m_onDisconnect->listeners().isEmpty())
        return;

    RELEASE_LOG_DEBUG(Extensions, "Fired port disconnect event for channel %{public}llu in %" PUBLIC_LOG_STRING " world", m_channelIdentifier ? m_channelIdentifier->toUInt64() : 0, toDebugString(contentWorldType()).utf8().data());

    // Copy the listeners since call() can trigger a mutation of the listeners.
    auto listenersCopy = m_onDisconnect->listeners();

    for (RefPtr listener : listenersCopy) {
        auto globalContext = listener->globalContext();
        if (!globalContext)
            continue;

        if (m_errorMessage) {
            protect(runtime())->reportError(*m_errorMessage, globalContext, [&] {
                listener->call(toJS(globalContext, this));
            });
        } else
            listener->call(toJS(globalContext, this));
    }

    m_onDisconnect->removeAllListeners();
}

WebExtensionAPIEvent& WebExtensionAPIPort::onMessage()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/Port#onmessage

    if (!m_onMessage)
        lazyInitialize(m_onMessage, WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::PortOnMessage));

    return *m_onMessage;
}

WebExtensionAPIEvent& WebExtensionAPIPort::onDisconnect()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/Port#ondisconnect

    if (!m_onDisconnect)
        lazyInitialize(m_onDisconnect, WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::PortOnDisconnect));

    return *m_onDisconnect;
}

void WebExtensionContextProxy::dispatchPortMessageEvent(std::optional<WebPageProxyIdentifier> sendingPageProxyIdentifier, WebExtensionPortChannelIdentifier channelIdentifier, const String& messageJSON, bool userGesture)
{
    auto ports = WebExtensionAPIPort::get(channelIdentifier);
    if (ports.isEmpty())
        return;

    for (Ref port : ports) {
        // Don't send the message to other ports in the same page as the sender.
        if (sendingPageProxyIdentifier && sendingPageProxyIdentifier == port->owningPageProxyIdentifier())
            continue;

        port->fireMessageEventIfNeeded(messageJSON, userGesture);
    }
}

void WebExtensionContextProxy::dispatchPortDisconnectEvent(WebExtensionPortChannelIdentifier channelIdentifier, const std::optional<String>& errorMessage)
{
    auto ports = WebExtensionAPIPort::get(channelIdentifier);
    if (ports.isEmpty())
        return;

    for (Ref port : ports)
        port->fireDisconnectEventIfNeeded(errorMessage);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
