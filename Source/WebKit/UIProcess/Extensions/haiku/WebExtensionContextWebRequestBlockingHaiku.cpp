/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "FormDataReference.h"
#include "Logging.h"
#include "ResourceLoadInfo.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebExtensionTab.h"
#include "WebExtensionWebRequestGateResultHaiku.h"
#include "WebExtensionWindow.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/FormData.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>

namespace WebKit {

namespace {

std::optional<WebExtensionEventListenerType> blockingListenerType(uint8_t phase)
{
    switch (phase) {
    case webExtensionWebRequestBeforeRequestPhaseHaiku:
        return WebExtensionEventListenerType::WebRequestOnBeforeRequest;
    case webExtensionWebRequestBeforeSendHeadersPhaseHaiku:
        return WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders;
    case webExtensionWebRequestHeadersReceivedPhaseHaiku:
        return WebExtensionEventListenerType::WebRequestOnHeadersReceived;
    case webExtensionWebRequestAuthRequiredPhaseHaiku:
        return WebExtensionEventListenerType::WebRequestOnAuthRequired;
    default:
        return std::nullopt;
    }
}

bool isBlockableListenerType(WebExtensionEventListenerType type)
{
    return type == WebExtensionEventListenerType::WebRequestOnBeforeRequest
        || type == WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders
        || type == WebExtensionEventListenerType::WebRequestOnHeadersReceived
        || type == WebExtensionEventListenerType::WebRequestOnAuthRequired;
}

std::optional<WebCore::FrameIdentifier> mainFrameIdentifierForBlockingWebRequest(WebPageProxyIdentifier pageIdentifier, const ResourceLoadInfo& resource)
{
    // Mirrors mainFrameIdentifierForWebRequest() used by the observation path.
    RefPtr page = WebProcessProxy::webPage(pageIdentifier);
    if (!page || page->isClosed())
        return std::nullopt;
    if (resource.parentFrameID) {
        RefPtr parent = WebFrameProxy::webFrame(*resource.parentFrameID);
        if (parent && parent->page() == page.get() && parent->isMainFrame())
            return parent->frameID();
    }
    RefPtr mainFrame = page->mainFrame();
    return mainFrame ? std::optional { mainFrame->frameID() } : std::nullopt;
}

// Replies with every collected (listener order, JSON) pair once the last
// process has answered or its connection has closed.
class BlockingReplyCollector : public RefCounted<BlockingReplyCollector> {
public:
    static Ref<BlockingReplyCollector> create(CompletionHandler<void(Vector<std::pair<uint64_t, String>>&&)>&& completionHandler)
    {
        return adoptRef(*new BlockingReplyCollector(WTF::move(completionHandler)));
    }

    ~BlockingReplyCollector()
    {
        m_completionHandler(WTF::move(m_replies));
    }

    void add(uint64_t processOrder, Vector<String>&& replies)
    {
        // Bound what one process can contribute; the merge rejects more than
        // 256 replies per transaction and 65535 listeners is far beyond that.
        static constexpr size_t maximumRepliesPerProcess = 256;
        for (size_t index = 0; index < replies.size() && index < maximumRepliesPerProcess; ++index) {
            if (replies[index].isEmpty())
                continue;
            m_replies.append({ (processOrder << 32) | static_cast<uint64_t>(index + 1), WTF::move(replies[index]) });
        }
    }

private:
    explicit BlockingReplyCollector(CompletionHandler<void(Vector<std::pair<uint64_t, String>>&&)>&& completionHandler)
        : m_completionHandler(WTF::move(completionHandler))
    {
    }

    CompletionHandler<void(Vector<std::pair<uint64_t, String>>&&)> m_completionHandler;
    Vector<std::pair<uint64_t, String>> m_replies;
};

}

void WebExtensionContext::addBlockingWebRequestListenerHaiku(WebCore::FrameIdentifier frameIdentifier, WebExtensionEventListenerType listenerType)
{
    if (!isBlockableListenerType(listenerType))
        return;

    // Blocking listeners live in this extension's own pages (background page,
    // popups, options): the message validator only accepts privileged
    // extension processes, and webRequest is not exposed to content scripts.
    RefPtr frame = WebFrameProxy::webFrame(frameIdentifier);
    if (!frame)
        return;

    auto result = m_blockingWebRequestListenerFramesHaiku.add(listenerType, WeakFrameCountedSet { });
    result.iterator->value.add(*frame);

    if (RefPtr controller = extensionController())
        controller->updateWebRequestBlockingPhasesHaiku();
}

void WebExtensionContext::removeBlockingWebRequestListenerHaiku(WebCore::FrameIdentifier frameIdentifier, WebExtensionEventListenerType listenerType, uint64_t removedCount)
{
    RefPtr frame = WebFrameProxy::webFrame(frameIdentifier);
    auto iterator = m_blockingWebRequestListenerFramesHaiku.find(listenerType);
    if (!frame || iterator == m_blockingWebRequestListenerFramesHaiku.end())
        return;

    for (uint64_t i = 0; i < removedCount && iterator->value.contains(*frame); ++i)
        iterator->value.remove(*frame);

    if (iterator->value.isEmptyIgnoringNullReferences())
        m_blockingWebRequestListenerFramesHaiku.remove(iterator);

    if (RefPtr controller = extensionController())
        controller->updateWebRequestBlockingPhasesHaiku();
}

bool WebExtensionContext::hasBlockingWebRequestListenersHaiku(WebExtensionEventListenerType listenerType) const
{
    if (!isLoaded())
        return false;
    auto iterator = m_blockingWebRequestListenerFramesHaiku.find(listenerType);
    return iterator != m_blockingWebRequestListenerFramesHaiku.end() && !iterator->value.isEmptyIgnoringNullReferences();
}

bool WebExtensionContext::canBlockWebRequestHaiku(WebPageProxyIdentifier pageIdentifier, const URL& resourceURL, const ResourceLoadInfo& loadInfo, uint8_t phase)
{
    // Conflict resolution needs the immutable installation identity/order the
    // native package registry assigns; unregistered contexts only observe.
    if (!nativeInstallationOrder() || nativeInstallationIdentifier().isEmpty())
        return false;

    // Same live tab/profile/host checks as observation, plus webRequestBlocking.
    RefPtr tab = getTab(pageIdentifier);
    if (!hasPermissionToSendWebRequestEvent(tab.get(), resourceURL, loadInfo))
        return false;
    if (hasPermission(String { "webRequestBlocking"_s }, tab.get()))
        return true;
    // MV3: webRequestAuthProvider allows answering onAuthRequired only.
    return phase == webExtensionWebRequestAuthRequiredPhaseHaiku && hasPermission(String { "webRequestAuthProvider"_s }, tab.get());
}

void WebExtensionContext::dispatchBlockingWebRequestHaiku(uint8_t phase, WebPageProxyIdentifier pageIdentifier, const ResourceLoadInfo& loadInfo, const WebCore::ResourceRequest& request,
    const std::optional<WebCore::ResourceResponse>& response, const std::optional<WebCore::AuthenticationChallenge>& challenge, CompletionHandler<void(Vector<std::pair<uint64_t, String>>&&)>&& completionHandler)
{
    auto listenerType = blockingListenerType(phase);
    if (!listenerType || !hasBlockingWebRequestListenersHaiku(*listenerType) || !canBlockWebRequestHaiku(pageIdentifier, request.url(), loadInfo, phase))
        return completionHandler({ });

    RefPtr tab = getTab(pageIdentifier);
    if (!tab)
        return completionHandler({ });

    auto& blockingFrames = m_blockingWebRequestListenerFramesHaiku.find(*listenerType)->value;
    auto targetProcesses = processes(EventListenerTypeSet { *listenerType }, ContentWorldTypeSet { WebExtensionContentWorldType::Main }, [&](WebProcessProxy&, WebPageProxy&, WebFrameProxy& frame) {
        return blockingFrames.contains(frame);
    });
    if (targetProcesses.isEmpty())
        return completionHandler({ });

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForBlockingWebRequest(pageIdentifier, loadInfo);

    std::optional<IPC::FormDataReference> formDataReference;
    if (phase == webExtensionWebRequestBeforeRequestPhaseHaiku) {
        if (RefPtr formData = request.httpBody()) {
            RefPtr resolvedFormData = formData->resolveBlobReferences().ptr();
            formDataReference = IPC::FormDataReference { WTF::move(resolvedFormData) };
        }
    }

    auto collector = BlockingReplyCollector::create(WTF::move(completionHandler));
    uint64_t processOrder = 0;
    for (Ref process : targetProcesses) {
        ++processOrder;
        process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchBlockingWebRequestHaiku(phase, tab->identifier(), windowIdentifier, loadInfo, request, formDataReference, response, challenge, mainFrameIdentifier),
            [collector, processOrder](Vector<String>&& replies) {
                collector->add(processOrder, WTF::move(replies));
            }, identifier());
    }
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
