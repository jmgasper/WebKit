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
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Logging.h"
#include "MessageSenderInlines.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionDeferredReply.h"
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionMessageTargetParameters.h"
#include "WebExtensionTab.h"
#include "WebExtensionTabParameters.h"
#include "WebExtensionUtilities.h"
#include "WebProcessProxy.h"
#include <wtf/Box.h>
#include <wtf/CallbackAggregator.h>

namespace WebKit {

void WebExtensionContext::runtimeGetBackgroundPage(CompletionHandler<void(std::expected<std::optional<WebCore::PageIdentifier>, WebExtensionError>&&)>&& completionHandler)
{
    auto pendingReply = deferWebExtensionReply(WTF::move(completionHandler), toWebExtensionError("runtime.getBackgroundPage()"_s, nullString(), "background content is unavailable"_s));
    wakeUpBackgroundContentIfNecessary([this, protectedThis = Ref { *this }, pendingReply = WTF::move(pendingReply)]() mutable {
        auto completionHandler = resumeWebExtensionReply(WTF::move(pendingReply));
        completionHandler(backgroundPageIdentifierInOwnProcess());
    });
}

void WebExtensionContext::runtimeReload()
{
    std::ignore = reload();
}

void WebExtensionContext::runtimeSendMessage(const String& extensionID, const String& messageJSON, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "runtime.sendMessage()"_s;

    if (!extensionID.isEmpty() && uniqueIdentifier() != extensionID) {
        // FIXME: <https://webkit.org/b/id269299> Add support for externally_connectable:ids.
        completionHandler(toWebExtensionError(apiName, "extensionID"_s, "cross-extension messaging is not supported"_s));
        return;
    }

    WebExtensionMessageSenderParameters completeSenderParameters = senderParameters;
    if (RefPtr tab = getTab(senderParameters.pageProxyIdentifier))
        completeSenderParameters.tabParameters = tab->parameters();
    else if (senderParameters.contentWorldType == WebExtensionContentWorldType::ContentScript) {
        RELEASE_LOG_ERROR(Extensions, "Tab not found for message for content script message");
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    // Don't propagate web page gestures from content scripts to extension pages. Only gestures
    // from extension-originated events (action.onClicked, commands.onCommand, menus.onClicked)
    // should curry — those carry explicit user intent directed at the extension.
    bool resolvedUserGesture = userGesture && senderParameters.contentWorldType != WebExtensionContentWorldType::ContentScript;
    constexpr auto targetContentWorldType = WebExtensionContentWorldType::Main;
    constexpr auto eventType = WebExtensionEventListenerType::RuntimeOnMessage;

    auto pendingReply = deferWebExtensionReply(WTF::move(completionHandler), toWebExtensionError(apiName, nullString(), "background content is unavailable"_s));
    wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }, pendingReply = WTF::move(pendingReply)]() mutable {
        auto completionHandler = resumeWebExtensionReply(WTF::move(pendingReply));
        auto mainWorldProcesses = processes(eventType, targetContentWorldType);
        if (mainWorldProcesses.isEmpty()) {
            completionHandler({ });
            return;
        }

        auto callbackAggregator = EagerCallbackAggregator<void(std::expected<String, WebExtensionError>)>::create(WTF::move(completionHandler), { });

        for (auto& process : mainWorldProcesses) {
            process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchRuntimeMessageEvent(targetContentWorldType, messageJSON, std::nullopt, completeSenderParameters, resolvedUserGesture), [callbackAggregator](String&& replyJSON) {
                // A null reply means no listeners replied. Don't call the callbackAggregator
                // to give other listeners in a different process a chance to reply.
                if (replyJSON.isNull())
                    return;

                callbackAggregator.get()(WTF::move(replyJSON));
            }, identifier());
        }
    });
}

void WebExtensionContext::runtimeConnect(const String& extensionID, WebExtensionPortChannelIdentifier channelIdentifier, const String& name, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "runtime.connect()"_s;

    const auto sourceContentWorldType = senderParameters.contentWorldType;
    constexpr auto targetContentWorldType = WebExtensionContentWorldType::Main;

    // Add 1 for the starting port here so disconnect will balance with a decrement.
    addPorts(sourceContentWorldType, targetContentWorldType, channelIdentifier, { senderParameters.pageProxyIdentifier });

    if (!extensionID.isEmpty() && uniqueIdentifier() != extensionID) {
        // FIXME: <https://webkit.org/b/id269299> Add support for externally_connectable:ids.
        completionHandler(toWebExtensionError(apiName, "extensionID"_s, "cross-extension messaging is not supported"_s));
        return;
    }

    WebExtensionMessageSenderParameters completeSenderParameters = senderParameters;
    if (RefPtr tab = getTab(senderParameters.pageProxyIdentifier))
        completeSenderParameters.tabParameters = tab->parameters();
    else if (senderParameters.contentWorldType == WebExtensionContentWorldType::ContentScript) {
        RELEASE_LOG_ERROR(Extensions, "Tab not found for message for content script port");
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    // Don't propagate web page gestures from content scripts to extension pages.
    bool resolvedUserGesture = userGesture && senderParameters.contentWorldType != WebExtensionContentWorldType::ContentScript;
    constexpr auto eventType = WebExtensionEventListenerType::RuntimeOnConnect;

    auto pendingReply = deferWebExtensionReply(WTF::move(completionHandler), toWebExtensionError(apiName, nullString(), "background content is unavailable"_s));
    wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }, pendingReply = WTF::move(pendingReply)]() mutable {
        auto completionHandler = resumeWebExtensionReply(WTF::move(pendingReply));
        auto mainWorldProcesses = processes(eventType, targetContentWorldType);
        if (mainWorldProcesses.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), "no runtime.onConnect listeners found"_s));
            return;
        }

        auto handledCount = Box<size_t>::create(0);
        size_t totalExpected = mainWorldProcesses.size();

        for (auto& process : mainWorldProcesses) {
            process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchRuntimeConnectEvent(targetContentWorldType, channelIdentifier, name, std::nullopt, completeSenderParameters, resolvedUserGesture), [=, this, protectedThis = Ref { *this }](HashCountedSet<WebPageProxyIdentifier>&& addedPortCounts) mutable {
                // Flip target and source worlds since we're adding the opposite side of the port connection, sending from target back to source.
                addPorts(targetContentWorldType, sourceContentWorldType, channelIdentifier, WTF::move(addedPortCounts));

                fireQueuedPortMessageEventsIfNeeded(targetContentWorldType, channelIdentifier);
                fireQueuedPortMessageEventsIfNeeded(sourceContentWorldType, channelIdentifier);

                firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);

                if (++*handledCount < totalExpected)
                    return;

                clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
                clearQueuedPortMessages(sourceContentWorldType, channelIdentifier);
            }, identifier());
        }

        completionHandler({ });
    });
}

void WebExtensionContext::runtimeWebPageSendMessage(const String& extensionID, const String& messageJSON, const WebExtensionMessageSenderParameters& senderParameters, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr extensionController = this->extensionController();
    if (!extensionController) {
        completionHandler({ });
        return;
    }

    RefPtr destinationExtension = extensionController->extensionContext(extensionID);
    RefPtr tab = getTab(senderParameters.pageProxyIdentifier);
    if (!destinationExtension || !tab) {
        callAfterRandomDelay([completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler({ });
        });

        return;
    }

    WebExtensionMessageSenderParameters completeSenderParameters = senderParameters;
    completeSenderParameters.tabParameters = tab->parameters();

    auto url = completeSenderParameters.url;
    auto validMatchPatterns = protect(destinationExtension->extension())->externallyConnectableMatchPatterns();
    if (!hasPermission(url, tab.get()) || !WebExtensionMatchPattern::patternsMatchURL(validMatchPatterns, url)) {
        callAfterRandomDelay([completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler({ });
        });

        return;
    }

    // Web pages are never trusted for gesture propagation to extension pages.
    constexpr bool resolvedUserGesture = false;
    constexpr auto eventType = WebExtensionEventListenerType::RuntimeOnMessageExternal;

    auto pendingReply = deferWebExtensionReply(WTF::move(completionHandler), std::expected<String, WebExtensionError> { });
    wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }, pendingReply = WTF::move(pendingReply)]() mutable {
        auto completionHandler = resumeWebExtensionReply(WTF::move(pendingReply));
        auto mainWorldProcesses = processes(eventType, WebExtensionContentWorldType::Main);
        if (mainWorldProcesses.isEmpty()) {
            completionHandler({ });
            return;
        }

        auto callbackAggregator = EagerCallbackAggregator<void(std::expected<String, WebExtensionError>)>::create(WTF::move(completionHandler), { });

        for (auto& process : mainWorldProcesses) {
            process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchRuntimeMessageEvent(WebExtensionContentWorldType::Main, messageJSON, std::nullopt, completeSenderParameters, resolvedUserGesture), [callbackAggregator](String&& replyJSON) {
                // A null reply means no listeners replied. Don't call the callbackAggregator
                // to give other listeners in a different process a chance to reply.
                if (replyJSON.isNull())
                    return;

                callbackAggregator.get()(WTF::move(replyJSON));
            }, identifier());
        }
    });
}

void WebExtensionContext::runtimeWebPageConnect(const String& extensionID, WebExtensionPortChannelIdentifier channelIdentifier, const String& name, const WebExtensionMessageSenderParameters& senderParameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "runtime.connect()"_s;

    constexpr auto sourceContentWorldType = WebExtensionContentWorldType::WebPage;
    constexpr auto targetContentWorldType = WebExtensionContentWorldType::Main;

    RefPtr extensionController = this->extensionController();
    if (!extensionController) {
        completionHandler({ });
        return;
    }

    RefPtr destinationExtension = extensionController->extensionContext(extensionID);
    RefPtr tab = getTab(senderParameters.pageProxyIdentifier);
    if (!destinationExtension || !tab) {
        callAfterRandomDelay([=, this, protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler({ });
            firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);
            clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
        });

        return;
    }

    WebExtensionMessageSenderParameters completeSenderParameters = senderParameters;
    completeSenderParameters.tabParameters = tab->parameters();

    auto url = completeSenderParameters.url;
    auto validMatchPatterns = protect(destinationExtension->extension())->externallyConnectableMatchPatterns();
    if (!hasPermission(url, tab.get()) || !WebExtensionMatchPattern::patternsMatchURL(validMatchPatterns, url)) {
        callAfterRandomDelay([=, this, protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler({ });
            firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);
            clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
        });

        return;
    }

    // Add 1 for the starting port here so disconnect will balance with a decrement.
    addPorts(sourceContentWorldType, targetContentWorldType, channelIdentifier, { senderParameters.pageProxyIdentifier });

    // Web pages are never trusted for gesture propagation to extension pages.
    constexpr bool resolvedUserGesture = false;
    constexpr auto eventType = WebExtensionEventListenerType::RuntimeOnConnectExternal;

    auto pendingReply = deferWebExtensionReply(WTF::move(completionHandler), toWebExtensionError(apiName, nullString(), "background content is unavailable"_s));
    wakeUpBackgroundContentIfNecessaryToFireEvents({ eventType }, [=, this, protectedThis = Ref { *this }, pendingReply = WTF::move(pendingReply)]() mutable {
        auto completionHandler = resumeWebExtensionReply(WTF::move(pendingReply));
        auto mainWorldProcesses = processes(eventType, targetContentWorldType);
        if (mainWorldProcesses.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), "no runtime.onConnectExternal listeners found"_s));
            return;
        }

        auto handledCount = Box<size_t>::create(0);
        size_t totalExpected = mainWorldProcesses.size();

        for (auto& process : mainWorldProcesses) {
            process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchRuntimeConnectEvent(targetContentWorldType, channelIdentifier, name, std::nullopt, completeSenderParameters, resolvedUserGesture), [=, this, protectedThis = Ref { *this }](HashCountedSet<WebPageProxyIdentifier>&& addedPortCounts) mutable {
                // Flip target and source worlds since we're adding the opposite side of the port connection, sending from target back to source.
                addPorts(targetContentWorldType, sourceContentWorldType, channelIdentifier, WTF::move(addedPortCounts));

                fireQueuedPortMessageEventsIfNeeded(targetContentWorldType, channelIdentifier);
                fireQueuedPortMessageEventsIfNeeded(sourceContentWorldType, channelIdentifier);

                firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);

                if (++*handledCount < totalExpected)
                    return;

                clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
                clearQueuedPortMessages(sourceContentWorldType, channelIdentifier);
            }, identifier());
        }

        completionHandler({ });
    });
}

void WebExtensionContext::fireRuntimeStartupEventIfNeeded()
{
    // The background content is assumed to be loaded for this event.

    RELEASE_LOG_DEBUG(Extensions, "Firing startup event");

    constexpr auto type = WebExtensionEventListenerType::RuntimeOnStartup;
    sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchRuntimeStartupEvent());
}

void WebExtensionContext::fireRuntimeInstalledEventIfNeeded()
{
    ASSERT(m_installReason != InstallReason::None);

    // The background content is assumed to be loaded for this event.

    RELEASE_LOG_DEBUG(Extensions, "Firing installed event");

    constexpr auto type = WebExtensionEventListenerType::RuntimeOnInstalled;
    sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchRuntimeInstalledEvent(m_installReason, m_previousVersion));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
