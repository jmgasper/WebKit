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
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Logging.h"
#include "MessageSenderInlines.h"
#include "WebExtensionContentWorldType.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"

namespace WebKit {

void WebExtensionContext::portPostMessage(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, std::optional<WebPageProxyIdentifier> sendingPageProxyIdentifier, WebExtensionPortChannelIdentifier channelIdentifier, const String& messageJSON, bool userGesture)
{
    if (sendingPageProxyIdentifier && isBackgroundPage(sendingPageProxyIdentifier.value()))
        m_lastBackgroundPortActivityTime = MonotonicTime::now();

    // Don't propagate gestures from content scripts or web pages to extension pages.
    bool resolvedUserGesture = userGesture
        && sourceContentWorldType != WebExtensionContentWorldType::ContentScript
        && sourceContentWorldType != WebExtensionContentWorldType::WebPage;

    if (!isPortConnected(sourceContentWorldType, targetContentWorldType, channelIdentifier)) {
        // The port might not be open on the other end yet. Queue the message until it does open.
        RELEASE_LOG_DEBUG(Extensions, "Enqueued message for port channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(targetContentWorldType).utf8().data());

        auto& messages = m_portQueuedMessages.ensure({ targetContentWorldType, channelIdentifier }, [] {
            return Vector<PortQueuedMessage> { };
        }).iterator->value;

        messages.append({ messageJSON, sendingPageProxyIdentifier, resolvedUserGesture });
        return;
    }

    firePortMessageEventsIfNeeded(targetContentWorldType, sendingPageProxyIdentifier, channelIdentifier, messageJSON, resolvedUserGesture);
}

void WebExtensionContext::portRemoved(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebPageProxyIdentifier webPageProxyIdentifier, WebExtensionPortChannelIdentifier channelIdentifier)
{
    removePort(sourceContentWorldType, targetContentWorldType, channelIdentifier, webPageProxyIdentifier);

    firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);
}

bool WebExtensionContext::pageHasOpenPorts(WebPageProxy& page)
{
    return m_pagePortMap.contains(page.identifier());
}

void WebExtensionContext::disconnectPortsForPage(WebPageProxy& page)
{
    auto portCounts = m_pagePortMap.take(page.identifier());
    if (portCounts.isEmpty())
        return;

    RELEASE_LOG_DEBUG(Extensions, "Disconnecting ports for page %{public}llu", page.identifier().toUInt64());

    for (auto& entry : portCounts) {
        auto sourceContentWorldType = std::get<0>(entry.key);
        auto targetContentWorldType = std::get<1>(entry.key);
        auto channelIdentifier = std::get<WebExtensionPortChannelIdentifier>(entry.key);

        for (size_t i = 0; i < entry.value; ++i)
            removePort(sourceContentWorldType, targetContentWorldType, channelIdentifier, page.identifier());

        firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);
    }
}

void WebExtensionContext::addPorts(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier channelIdentifier, HashCountedSet<WebPageProxyIdentifier>&& addedPortCounts)
{
    for (auto& entry : addedPortCounts) {
        RELEASE_LOG_DEBUG(Extensions, "Added %{public}u port(s) for channel %{public}llu in %" PUBLIC_LOG_STRING " world for page %{public}llu", entry.value, channelIdentifier.toUInt64(), toDebugString(sourceContentWorldType).utf8().data(), entry.key.toUInt64());

        if (isBackgroundPage(entry.key))
            m_lastBackgroundPortActivityTime = MonotonicTime::now();

        m_ports.add({ sourceContentWorldType, channelIdentifier }, entry.value);

        m_pagePortMap.add(entry.key, { sourceContentWorldType, targetContentWorldType, channelIdentifier }, entry.value);
    }
}

void WebExtensionContext::removePort(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier channelIdentifier, WebPageProxyIdentifier webPageProxyIdentifier)
{
    RELEASE_LOG_DEBUG(Extensions, "Removed 1 port for channel %{public}llu in %" PUBLIC_LOG_STRING " world for page %{public}llu", channelIdentifier.toUInt64(), toDebugString(sourceContentWorldType).utf8().data(), webPageProxyIdentifier.toUInt64());

    if (m_ports.remove({ sourceContentWorldType, channelIdentifier }))
        clearQueuedPortMessages(targetContentWorldType, channelIdentifier);

    m_pagePortMap.remove(webPageProxyIdentifier, { sourceContentWorldType, targetContentWorldType, channelIdentifier });
}

bool WebExtensionContext::addNativePort(WebExtensionMessagePort& nativePort)
{
    constexpr auto contentWorldType = WebExtensionContentWorldType::Native;
    auto channelIdentifier = nativePort.channelIdentifier();

    RELEASE_LOG_DEBUG(Extensions, "Added 1 port for channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(contentWorldType).utf8().data());

    if (!isLoaded() || nativePort.extensionContext() != this || nativePort.isDisconnected())
        return false;
    if (!m_nativePortMap.add(channelIdentifier, nativePort).isNewEntry)
        return false;
    m_ports.add({ contentWorldType, channelIdentifier });
    return true;
}

void WebExtensionContext::removeNativePort(WebExtensionMessagePort& nativePort)
{
    constexpr auto contentWorldType = WebExtensionContentWorldType::Native;
    auto channelIdentifier = nativePort.channelIdentifier();

    RefPtr currentPort = m_nativePortMap.get(channelIdentifier);
    if (currentPort.get() != &nativePort)
        return;

    RELEASE_LOG_DEBUG(Extensions, "Removed 1 port for channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(contentWorldType).utf8().data());

    clearQueuedPortMessages(WebExtensionContentWorldType::Main, channelIdentifier);
    clearQueuedPortMessages(WebExtensionContentWorldType::Native, channelIdentifier);

    m_ports.remove({ contentWorldType, channelIdentifier });
    m_nativePortMap.remove(channelIdentifier);
}

unsigned WebExtensionContext::openPortCount(WebExtensionContentWorldType contentWorldType, WebExtensionPortChannelIdentifier channelIdentifier)
{
    auto count = m_ports.count({ contentWorldType, channelIdentifier });

#if ENABLE(INSPECTOR_EXTENSIONS)
    // Inspector content world is a special alias of Main. Include it when Main is requested (and vice versa).
    if (contentWorldType == WebExtensionContentWorldType::Main)
        count += m_ports.count({ WebExtensionContentWorldType::Inspector, channelIdentifier });
    else if (contentWorldType == WebExtensionContentWorldType::Inspector)
        count += m_ports.count({ WebExtensionContentWorldType::Main, channelIdentifier });
#endif

    return count;
}

bool WebExtensionContext::isPortConnected(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier channelIdentifier)
{
    const auto sourceWorldCount = openPortCount(sourceContentWorldType, channelIdentifier);

    RELEASE_LOG_DEBUG(Extensions, "Port channel %{public}llu has %{public}u port(s) open in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), sourceWorldCount, toDebugString(sourceContentWorldType).utf8().data());

    // When the worlds are the same, it is connected if there are 2 ports remaining.
    if (isEqual(sourceContentWorldType, targetContentWorldType))
        return sourceWorldCount >= 2;

    const auto targetWorldCount = openPortCount(targetContentWorldType, channelIdentifier);

    RELEASE_LOG_DEBUG(Extensions, "Port channel %{public}llu has %{public}u port(s) open in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), targetWorldCount, toDebugString(targetContentWorldType).utf8().data());

    // When the worlds are different, it is connected if there is 1 port in each world remaining.
    return sourceWorldCount && targetWorldCount;
}

Vector<WebExtensionContext::PortQueuedMessage> WebExtensionContext::portQueuedMessages(WebExtensionContentWorldType contentWorldType, WebExtensionPortChannelIdentifier channelIdentifier)
{
    auto messages = m_portQueuedMessages.get({ contentWorldType, channelIdentifier });

#if ENABLE(INSPECTOR_EXTENSIONS)
    // Inspector content world is a special alias of Main. Include it when Main is requested (and vice versa).
    if (contentWorldType == WebExtensionContentWorldType::Main)
        messages.appendVector(m_portQueuedMessages.get({ WebExtensionContentWorldType::Inspector, channelIdentifier }));
    else if (contentWorldType == WebExtensionContentWorldType::Inspector)
        messages.appendVector(m_portQueuedMessages.get({ WebExtensionContentWorldType::Main, channelIdentifier }));
#endif

    return messages;
}

void WebExtensionContext::fireQueuedPortMessageEventsIfNeeded(WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier channelIdentifier)
{
    auto messages = portQueuedMessages(targetContentWorldType, channelIdentifier);
    if (messages.isEmpty())
        return;

    RELEASE_LOG_DEBUG(Extensions, "Sending %{public}zu queued message(s) to port channel %{public}llu in %" PUBLIC_LOG_STRING " world", messages.size(), channelIdentifier.toUInt64(), toDebugString(targetContentWorldType).utf8().data());

    for (auto& entry : messages)
        firePortMessageEventsIfNeeded(targetContentWorldType, entry.sendingPageProxyIdentifier, channelIdentifier, entry.messageJSON, entry.userGesture);
}

void WebExtensionContext::clearQueuedPortMessages(WebExtensionContentWorldType contentWorldType, WebExtensionPortChannelIdentifier channelIdentifier)
{
    if (m_portQueuedMessages.remove({ contentWorldType, channelIdentifier }))
        RELEASE_LOG_DEBUG(Extensions, "Cleared queued message(s) for port channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(contentWorldType).utf8().data());

#if ENABLE(INSPECTOR_EXTENSIONS)
    // Inspector content world is a special alias of Main. Include it when Main is requested (and vice versa).
    if (contentWorldType == WebExtensionContentWorldType::Main)
        m_portQueuedMessages.remove({ WebExtensionContentWorldType::Inspector, channelIdentifier });
    else if (contentWorldType == WebExtensionContentWorldType::Inspector)
        m_portQueuedMessages.remove({ WebExtensionContentWorldType::Main, channelIdentifier });
#endif
}

void WebExtensionContext::firePortDisconnectEventIfNeeded(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier channelIdentifier, const std::optional<String>& errorMessage)
{
    // Only fire the disconnect event if there are no ports connected on this channel.
    if (isPortConnected(sourceContentWorldType, targetContentWorldType, channelIdentifier))
        return;

    RELEASE_LOG_DEBUG(Extensions, "Sending disconnect event for port channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(targetContentWorldType).utf8().data());

    if (targetContentWorldType == WebExtensionContentWorldType::Native) {
        if (RefPtr nativePort = m_nativePortMap.get(channelIdentifier))
            nativePort->reportDisconnection(errorMessage ? WebExtensionMessagePort::Error { { WebExtensionMessagePort::ErrorType::Unknown, *errorMessage } } : std::nullopt);
        return;
    }

    // Disconnection changes port state even when no onDisconnect listener exists.
    // Include every process of each owning page, including site-isolated frames.
    WebProcessProxySet recipients;
    for (auto pageIdentifier : m_pagePortMap.pages(targetContentWorldType, channelIdentifier)) {
        RefPtr page = WebProcessProxy::webPage(pageIdentifier);
        if (!page || page->isClosed())
            continue;
        page->forEachWebContentProcess([&](WebProcessProxy& process, auto) {
            if (process.canSendMessage())
                recipients.add(process);
        });
    }
    sendToProcesses(recipients, Messages::WebExtensionContextProxy::DispatchPortDisconnectEvent(channelIdentifier, errorMessage));
}

void WebExtensionContext::firePortMessageEventsIfNeeded(WebExtensionContentWorldType targetContentWorldType, std::optional<WebPageProxyIdentifier> sendingPageProxyIdentifier, WebExtensionPortChannelIdentifier channelIdentifier, const String& messageJSON, bool userGesture)
{
    RELEASE_LOG_DEBUG(Extensions, "Sending message to port channel %{public}llu in %" PUBLIC_LOG_STRING " world", channelIdentifier.toUInt64(), toDebugString(targetContentWorldType).utf8().legacyCStringPointer());

    constexpr auto type = WebExtensionEventListenerType::PortOnMessage;

    switch (targetContentWorldType) {
    case WebExtensionContentWorldType::Main:
#if ENABLE(INSPECTOR_EXTENSIONS)
    case WebExtensionContentWorldType::Inspector:
#endif
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchPortMessageEvent(sendingPageProxyIdentifier, channelIdentifier, messageJSON, userGesture));
        return;

    case WebExtensionContentWorldType::ContentScript:
        sendToContentScriptProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchPortMessageEvent(sendingPageProxyIdentifier, channelIdentifier, messageJSON, userGesture));
        return;

    case WebExtensionContentWorldType::Native:
#if PLATFORM(COCOA) || PLATFORM(HAIKU)
        dispatchNativePortMessage(channelIdentifier, messageJSON);
#endif
        return;

    case WebExtensionContentWorldType::WebPage:
        sendToProcesses(processes(type, WebExtensionContentWorldType::WebPage), Messages::WebExtensionContextProxy::DispatchPortMessageEvent(sendingPageProxyIdentifier, channelIdentifier, messageJSON, userGesture));
        return;
    }
}

#if PLATFORM(HAIKU)
void WebExtensionContext::dispatchNativePortMessage(WebExtensionPortChannelIdentifier channelIdentifier, const String& messageJSON)
{
    if (RefPtr nativePort = m_nativePortMap.get(channelIdentifier))
        nativePort->receiveMessages({ messageJSON });
}

void WebExtensionContext::sendQueuedNativePortMessagesIfNeeded(WebExtensionPortChannelIdentifier channelIdentifier)
{
    RefPtr nativePort = m_nativePortMap.get(channelIdentifier);
    if (!nativePort || nativePort->isDisconnected())
        return;
    // Take the whole batch before calling the host. The port queues the batch
    // together so reentrant messages follow every earlier pending message.
    auto messages = m_portQueuedMessages.take({ WebExtensionContentWorldType::Native, channelIdentifier });
    Vector<String> payloads;
    payloads.reserveInitialCapacity(messages.size());
    for (auto& entry : messages)
        payloads.append(WTF::move(entry.messageJSON));
    nativePort->receiveMessages(WTF::move(payloads));
}
#endif

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
