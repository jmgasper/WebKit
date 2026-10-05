/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionController.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "FormDataReference.h"
#include "Logging.h"
#include "NetworkProcessMessages.h"
#include "NetworkProcessProxy.h"
#include "ResourceLoadInfo.h"
#include "WebExtensionContext.h"
#include "WebExtensionWebRequestDecisionHaiku.h"
#include "WebExtensionWebRequestGateResultHaiku.h"
#include "WebExtensionWebRequestMergeHaiku.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebsiteDataStore.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/FormData.h>
#include <WebCore/HTTPHeaderMap.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/StdLibExtras.h>

namespace WebKit {

static_assert(static_cast<uint8_t>(WebExtensionWebRequestPhaseHaiku::BeforeRequest) == webExtensionWebRequestBeforeRequestPhaseHaiku);
static_assert(static_cast<uint8_t>(WebExtensionWebRequestPhaseHaiku::BeforeSendHeaders) == webExtensionWebRequestBeforeSendHeadersPhaseHaiku);
static_assert(static_cast<uint8_t>(WebExtensionWebRequestPhaseHaiku::HeadersReceived) == webExtensionWebRequestHeadersReceivedPhaseHaiku);
static_assert(static_cast<uint8_t>(WebExtensionWebRequestPhaseHaiku::AuthRequired) == webExtensionWebRequestAuthRequiredPhaseHaiku);

namespace {

constexpr std::array<WebExtensionEventListenerType, 4> blockingListenerTypes {
    WebExtensionEventListenerType::WebRequestOnBeforeRequest,
    WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders,
    WebExtensionEventListenerType::WebRequestOnHeadersReceived,
    WebExtensionEventListenerType::WebRequestOnAuthRequired,
};

uint8_t& processWideBlockingPhases()
{
    static uint8_t phases { 0 };
    return phases;
}

HashMap<WebExtensionControllerIdentifier, uint8_t>& controllerBlockingPhases()
{
    static MainRunLoopNeverDestroyed<HashMap<WebExtensionControllerIdentifier, uint8_t>> phases;
    return phases;
}

Vector<WebExtensionWebRequestHeaderHaiku> headersForMerge(const WebCore::HTTPHeaderMap& headers)
{
    Vector<WebExtensionWebRequestHeaderHaiku> result;
    for (auto& header : headers) {
        auto utf8 = header.value.utf8();
        Vector<uint8_t> value;
        value.append(byteCast<uint8_t>(utf8.span()));
        result.append({ header.key, WTF::move(value) });
    }
    return result;
}

String headerValueString(const Vector<uint8_t>& value)
{
    auto text = String::fromUTF8(byteCast<char8_t>(value.span()));
    if (!text.isNull())
        return text;
    // binaryValue need not be UTF-8; HTTP header octets map to Latin-1.
    return String::fromLatin1(byteCast<char>(value.span()));
}

// One blocking decision for one phase of one request, spanning every eligible
// extension. Completes exactly once: when every extension has replied (or its
// processes went away), or when the listener timeout fires, whichever is first.
class BlockingTransaction : public RefCounted<BlockingTransaction> {
public:
    static Ref<BlockingTransaction> create(uint8_t phase, URL&& url, Vector<WebExtensionWebRequestHeaderHaiku>&& originalHeaders, CompletionHandler<void(WebExtensionWebRequestGateResultHaiku&&)>&& completionHandler)
    {
        return adoptRef(*new BlockingTransaction(phase, WTF::move(url), WTF::move(originalHeaders), WTF::move(completionHandler)));
    }

    ~BlockingTransaction()
    {
        finish();
    }

    void addReplies(const String& installationIdentifier, uint64_t installationOrder, Vector<std::pair<uint64_t, String>>&& replies)
    {
        if (m_finished)
            return;
        for (auto& [listenerOrder, json] : replies) {
            // Invalid replies are ignored, as in Chrome: they never mutate the load.
            auto decision = parseWebExtensionWebRequestDecisionHaiku(json);
            if (!decision) {
                RELEASE_LOG_ERROR(Extensions, "Ignoring invalid blocking webRequest reply: %" PUBLIC_LOG_STRING, decision.error().utf8().data());
                continue;
            }
            if (!decision->cancel && !decision->redirectURL && !decision->requestHeaders && !decision->responseHeaders && !decision->authCredentials)
                continue;
            WebExtensionWebRequestReplyHaiku reply;
            reply.installationIdentifier = installationIdentifier;
            reply.installationOrder = installationOrder;
            reply.listenerOrder = listenerOrder;
            reply.decision = WTF::move(*decision);
            m_replies.append(WTF::move(reply));
        }
    }

    void startTimeout()
    {
        if (!m_finished)
            m_timeout.startOneShot(webExtensionWebRequestBlockingTimeoutHaiku);
    }

private:
    BlockingTransaction(uint8_t phase, URL&& url, Vector<WebExtensionWebRequestHeaderHaiku>&& originalHeaders, CompletionHandler<void(WebExtensionWebRequestGateResultHaiku&&)>&& completionHandler)
        : m_phase(phase)
        , m_url(WTF::move(url))
        , m_originalHeaders(WTF::move(originalHeaders))
        , m_completionHandler(WTF::move(completionHandler))
        , m_timeout(RunLoop::mainSingleton(), "WebExtensionWebRequestBlockingTimeout"_s, [this] {
            RELEASE_LOG_ERROR(Extensions, "Blocking webRequest listeners did not answer in time; continuing with the replies received so far");
            finish();
        })
    {
    }

    void finish()
    {
        if (m_finished)
            return;
        m_finished = true;
        m_timeout.stop();
        m_completionHandler(resolve());
    }

    WebExtensionWebRequestGateResultHaiku resolve()
    {
        WebExtensionWebRequestGateResultHaiku result;
        if (m_replies.isEmpty())
            return result;

        // Keep within the merge bounds rather than failing the whole decision.
        static constexpr size_t maximumReplies = 256;
        if (m_replies.size() > maximumReplies)
            m_replies.shrink(maximumReplies);

        auto merged = mergeWebExtensionWebRequestRepliesHaiku(static_cast<WebExtensionWebRequestPhaseHaiku>(m_phase), m_url, m_originalHeaders, { }, m_replies);
        if (!merged) {
            RELEASE_LOG_ERROR(Extensions, "Blocking webRequest replies could not be merged, continuing unmodified: %" PUBLIC_LOG_STRING, merged.error().utf8().data());
            return result;
        }

        if (merged->cancel) {
            result.cancel = true;
            return result;
        }

        if (merged->redirectURL && m_phase == webExtensionWebRequestBeforeRequestPhaseHaiku) {
            auto& destination = *merged->redirectURL;
            if (destination.protocolIsInHTTPFamily()) {
                if (destination != m_url)
                    result.redirectURL = destination;
            } else {
                // The network process can only follow HTTP(S) redirects.
                // data:/about:blank are Chrome's cancellation-style redirects;
                // extension resources (web_accessible_resources surrogates) and
                // other schemes are blocked rather than silently allowed.
                RELEASE_LOG(Extensions, "Blocking webRequest redirect to a non-HTTP(S) URL is applied as cancellation");
                result.cancel = true;
                return result;
            }
        }

        if (merged->headers && (m_phase == webExtensionWebRequestBeforeSendHeadersPhaseHaiku || m_phase == webExtensionWebRequestHeadersReceivedPhaseHaiku)) {
            Vector<std::pair<String, String>> headers;
            headers.reserveInitialCapacity(merged->headers->size());
            for (auto& header : *merged->headers)
                headers.append({ header.name, headerValueString(header.value) });
            result.headers = WTF::move(headers);
        }

        if (merged->authCredentials && m_phase == webExtensionWebRequestAuthRequiredPhaseHaiku)
            result.credentials = std::pair { merged->authCredentials->username, merged->authCredentials->password };

        if (!merged->ignored.isEmpty())
            RELEASE_LOG(Extensions, "%zu blocking webRequest modification(s) lost a conflict with a more recently installed extension", merged->ignored.size());

        return result;
    }

    const uint8_t m_phase;
    const URL m_url;
    const Vector<WebExtensionWebRequestHeaderHaiku> m_originalHeaders;
    Vector<WebExtensionWebRequestReplyHaiku> m_replies;
    CompletionHandler<void(WebExtensionWebRequestGateResultHaiku&&)> m_completionHandler;
    RunLoop::Timer m_timeout;
    bool m_finished { false };
};

}

void WebExtensionController::decideWebRequestHaiku(WebPageProxyIdentifier pageIdentifier, uint8_t phase, ResourceLoadInfo&& loadInfo, WebCore::ResourceRequest&& request,
    std::optional<WebCore::ResourceResponse>&& response, std::optional<WebCore::AuthenticationChallenge>&& challenge, CompletionHandler<void(WebExtensionWebRequestGateResultHaiku&&)>&& completionHandler)
{
    if (phase > webExtensionWebRequestLastPhaseHaiku)
        return completionHandler({ });
    auto listenerType = blockingListenerTypes[phase];

    Vector<Ref<WebExtensionContext>> eligibleContexts;
    bool hasBlockingListener = false;
    for (Ref context : m_extensionContexts) {
        if (context->extensionController() != this || !context->hasBlockingWebRequestListenersHaiku(listenerType))
            continue;
        hasBlockingListener = true;
        if (context->canBlockWebRequestHaiku(pageIdentifier, request.url(), loadInfo, phase))
            eligibleContexts.append(WTF::move(context));
    }

    // The network process' phase mask is only a hint; repair it when the
    // listeners it advertises have gone away without an explicit removal.
    if (!hasBlockingListener)
        updateWebRequestBlockingPhasesHaiku();
    if (eligibleContexts.isEmpty())
        return completionHandler({ });

    Vector<WebExtensionWebRequestHeaderHaiku> originalHeaders;
    if (phase == webExtensionWebRequestBeforeSendHeadersPhaseHaiku)
        originalHeaders = headersForMerge(request.httpHeaderFields());
    else if (phase == webExtensionWebRequestHeadersReceivedPhaseHaiku && response)
        originalHeaders = headersForMerge(response->httpHeaderFields());

    auto transaction = BlockingTransaction::create(phase, URL { request.url() }, WTF::move(originalHeaders), WTF::move(completionHandler));
    for (Ref context : eligibleContexts) {
        auto installationIdentifier = context->nativeInstallationIdentifier();
        auto installationOrder = context->nativeInstallationOrder();
        context->dispatchBlockingWebRequestHaiku(phase, pageIdentifier, loadInfo, request, response, challenge,
            [transaction, weakContext = WeakPtr { context.get() }, installationIdentifier, installationOrder, pageIdentifier, phase, url = request.url(), loadInfo](Vector<std::pair<uint64_t, String>>&& replies) mutable {
                // Re-authorize on receipt: the extension may have been unloaded,
                // replaced or lost access while its listeners were running.
                RefPtr context = weakContext.get();
                if (!context || context->nativeInstallationOrder() != installationOrder || context->nativeInstallationIdentifier() != installationIdentifier
                    || !context->canBlockWebRequestHaiku(pageIdentifier, url, loadInfo, phase))
                    return;
                transaction->addReplies(installationIdentifier, installationOrder, WTF::move(replies));
            });
    }
    transaction->startTimeout();
}

void WebExtensionController::updateWebRequestBlockingPhasesHaiku()
{
    uint8_t phases = 0;
    for (Ref context : m_extensionContexts) {
        if (context->extensionController() != this)
            continue;
        bool blocking = context->hasPermission(String { "webRequestBlocking"_s });
        bool authProvider = context->hasPermission(String { "webRequestAuthProvider"_s });
        if (!blocking && !authProvider)
            continue;
        for (uint8_t phase = 0; phase <= webExtensionWebRequestLastPhaseHaiku; ++phase) {
            if (!blocking && phase != webExtensionWebRequestAuthRequiredPhaseHaiku)
                continue;
            if (context->hasBlockingWebRequestListenersHaiku(blockingListenerTypes[phase]))
                phases |= webExtensionWebRequestPhaseBitHaiku(phase);
        }
    }

    auto& controllers = controllerBlockingPhases();
    if (phases)
        controllers.set(identifier(), phases);
    else
        controllers.remove(identifier());

    uint8_t processWidePhases = 0;
    controllers.removeIf([&](auto& entry) {
        if (!WebExtensionController::get(entry.key))
            return true;
        processWidePhases |= entry.value;
        return false;
    });
    NetworkProcessProxy::setWebRequestBlockingPhasesHaiku(processWidePhases);
}

void NetworkProcessProxy::setWebRequestBlockingPhasesHaiku(uint8_t phases)
{
    if (processWideBlockingPhases() == phases)
        return;
    processWideBlockingPhases() = phases;
    for (Ref networkProcess : allNetworkProcesses())
        networkProcess->send(Messages::NetworkProcess::SetWebRequestBlockingPhasesHaiku(phases), 0);
}

uint8_t NetworkProcessProxy::webRequestBlockingPhasesHaiku()
{
    return processWideBlockingPhases();
}

void NetworkProcessProxy::resourceLoadWebRequestDecisionHaiku(WebPageProxyIdentifier pageIdentifier, uint8_t phase, ResourceLoadInfo&& loadInfo, WebCore::ResourceRequest&& request,
    std::optional<IPC::FormDataReference>&& httpBody, std::optional<WebCore::ResourceResponse>&& response, std::optional<WebCore::AuthenticationChallenge>&& challenge,
    CompletionHandler<void(WebExtensionWebRequestGateResultHaiku&&)>&& completionHandler)
{
    MESSAGE_CHECK_COMPLETION_BASE(phase <= webExtensionWebRequestLastPhaseHaiku, connection(), completionHandler({ }));

    // Every early return continues the load unmodified (fail open).
    RefPtr page = WebProcessProxy::webPage(pageIdentifier);
    if (!page || page->isClosed())
        return completionHandler({ });

    // A network process may only ask about pages whose data store it serves,
    // and about frames of that page.
    if (page->websiteDataStore().networkProcessIfExists() != this)
        return completionHandler({ });
    if (loadInfo.frameID) {
        RefPtr frame = WebFrameProxy::webFrame(*loadInfo.frameID);
        if (frame && frame->page() != page.get())
            return completionHandler({ });
    }

    RefPtr controller = page->webExtensionController();
    if (!controller)
        return completionHandler({ });

    if (httpBody)
        request.setHTTPBody(httpBody->takeData());

    controller->decideWebRequestHaiku(pageIdentifier, phase, WTF::move(loadInfo), WTF::move(request), WTF::move(response), WTF::move(challenge), WTF::move(completionHandler));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
