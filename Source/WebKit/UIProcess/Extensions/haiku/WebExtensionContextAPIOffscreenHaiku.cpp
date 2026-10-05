/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && PLATFORM(HAIKU)

#include "APIError.h"
#include "APINavigationAction.h"
#include "APINavigationClient.h"
#include "APIPageConfiguration.h"
#include "MessageSenderInlines.h"
#include "ProcessActivityGroup.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionOffscreenDocumentParameters.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include "WebFramePolicyListenerProxy.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include <WebCore/ResourceRequest.h>

// chrome.offscreen (MV3). Port of UIProcess/Extensions/Cocoa/API/
// WebExtensionContextAPIOffscreenCocoa.mm: the offscreen document is a hidden
// extension page created through the same native page client as the
// background page. MV3 extensions use it for clipboard access, DOMParser,
// localStorage and audio, none of which a service worker has.

namespace WebKit {

class ExtensionOffscreenNavigationClientHaiku final : public API::NavigationClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ExtensionOffscreenNavigationClientHaiku);
public:
    explicit ExtensionOffscreenNavigationClientHaiku(WebExtensionContext& context)
        : m_context(context) { }

private:
    RefPtr<WebExtensionContext> contextForPage(WebPageProxy& page) const
    {
        auto* context = m_context.get();
        if (!context || !context->isLoaded() || !context->isOffscreenWebView(page))
            return nullptr;
        return context;
    }

    void decidePolicyForNavigationAction(WebPageProxy& page, Ref<API::NavigationAction>&& action, Ref<WebFramePolicyListenerProxy>&& listener) final
    {
        // Like the background page, the main document stays inside the extension;
        // subframes (IFRAME_SCRIPTING) may load web content.
        RefPtr context = contextForPage(page);
        RefPtr targetFrame = action->targetFrame();
        if (context && ((targetFrame && !targetFrame->isMainFrame()) || context->isURLForThisExtension(action->request().url())))
            listener->use();
        else
            listener->ignore();
    }

    void didFinishDocumentLoad(WebPageProxy& page, API::Navigation*, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->offscreenDocumentDidFinishLoad(page);
    }

    void didFailProvisionalNavigationWithError(WebPageProxy& page, FrameInfoData&&, API::Navigation*, const URL&, const WebCore::ResourceError& error, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->offscreenDocumentDidFail(page, error.localizedDescription());
    }

    void didFailNavigationWithError(WebPageProxy& page, const FrameInfoData&, API::Navigation*, const URL&, const WebCore::ResourceError& error, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->offscreenDocumentDidFail(page, error.localizedDescription());
    }

    bool processDidTerminate(WebPageProxy& page, ProcessTerminationReason) final
    {
        if (RefPtr context = contextForPage(page))
            context->offscreenDocumentDidFail(page, "the offscreen document's process terminated"_s);
        return true;
    }

    WeakPtr<WebExtensionContext> m_context;
};

bool WebExtensionContext::isOffscreenMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasPermission(WebExtensionPermission::offscreen());
}

void WebExtensionContext::offscreenCreateDocument(const WebExtensionOffscreenDocumentParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "offscreen.createDocument()"_s;

    if (m_offscreenWebView) {
        completionHandler(toWebExtensionError(apiName, nullString(), "only a single offscreen document may be created"_s));
        return;
    }

    URL documentURL { m_baseURL, parameters.url };
    if (!isURLForThisExtension(documentURL)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "url must be a resource within the extension"_s));
        return;
    }

    if (!extensionController()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
        return;
    }

    RefPtr configuration = webViewConfiguration(WebViewPurpose::Offscreen);
    if (!configuration) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the offscreen document could not be created"_s));
        return;
    }

    m_offscreenPageClient = WebView::createForExtensionBackground(configuration.releaseNonNull());
    m_offscreenWebView = m_offscreenPageClient->page();
    Ref page = *m_offscreenWebView;
    page->setNavigationClient(makeUniqueRef<ExtensionOffscreenNavigationClientHaiku>(*this));
    page->setInspectable(m_inspectable);

    // The WebProcess limits an offscreen document to the runtime namespace.
    page->forEachWebContentProcess([&](auto& process, auto pageID) {
        process.send(Messages::WebExtensionContextProxy::SetOffscreenPageIdentifier(pageID), identifier());
    });

    constexpr auto activityName = "Web Extension offscreen document"_s;
    m_offscreenWebViewActivity = protect(page->activityGroupContext())->foregroundProcessActivityGroup(activityName);

    m_offscreenDocumentLoadCompletionHandlers.append(WTF::move(completionHandler));
    page->loadRequest(WebCore::ResourceRequest { URL { documentURL } });
}

void WebExtensionContext::offscreenCloseDocument(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "offscreen.closeDocument()"_s;

    if (!m_offscreenWebView) {
        completionHandler(toWebExtensionError(apiName, nullString(), "no offscreen document is open"_s));
        return;
    }

    unloadOffscreenWebView();
    completionHandler({ });
}

void WebExtensionContext::offscreenHasDocument(CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&& completionHandler)
{
    completionHandler(!!m_offscreenWebView);
}

void WebExtensionContext::offscreenDocumentDidFinishLoad(WebPageProxy& page)
{
    if (!isOffscreenWebView(page))
        return;
    performTasksAfterOffscreenContentLoads();
}

void WebExtensionContext::offscreenDocumentDidFail(WebPageProxy& page, const String& reason)
{
    if (!isOffscreenWebView(page))
        return;
    static constexpr auto apiName = "offscreen.createDocument()"_s;
    for (auto& completionHandler : std::exchange(m_offscreenDocumentLoadCompletionHandlers, { }))
        completionHandler(toWebExtensionError(apiName, nullString(), reason.isEmpty() ? "the offscreen document failed to load"_s : reason));
    unloadOffscreenWebView();
}

void WebExtensionContext::unloadOffscreenWebView()
{
    static constexpr auto completionHandlerAPIName = "offscreen.createDocument()"_s;
    for (auto& completionHandler : std::exchange(m_offscreenDocumentLoadCompletionHandlers, { }))
        completionHandler(toWebExtensionError(completionHandlerAPIName, nullString(), "offscreen document was closed"_s));

    m_offscreenWebViewActivity = { };
    // Clear identity before closing: close can re-enter the navigation client.
    m_offscreenWebView = nullptr;
    if (RefPtr client = std::exchange(m_offscreenPageClient, nullptr))
        client->close();
}

void WebExtensionContext::performTasksAfterOffscreenContentLoads()
{
    for (auto& completionHandler : std::exchange(m_offscreenDocumentLoadCompletionHandlers, { }))
        completionHandler({ });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && PLATFORM(HAIKU)
