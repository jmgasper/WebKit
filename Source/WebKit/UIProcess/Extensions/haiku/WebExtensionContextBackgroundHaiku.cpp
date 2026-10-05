/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionTimingHaiku.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "APIFrameInfo.h"
#include "APINavigationAction.h"
#include "APINavigationClient.h"
#include "APIPageConfiguration.h"
#include "APIWebsitePolicies.h"
#include "Logging.h"
#include "MessageSenderInlines.h"
#include "ProcessActivityGroup.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionBackgroundListenersHaiku.h"
#include "WebExtensionPermission.h"
#include "WebFramePolicyListenerProxy.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessProxy.h"
#include "WebProcessPool.h"
#include "WebViewPrivate.h"
#include <WebCore/ContentSecurityPolicy.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ShouldRelaxThirdPartyCookieBlocking.h>
#include <WebCore/UserAgent.h>
#include <cstdlib>
#include <cstring>

namespace WebKit {

class ExtensionBackgroundNavigationClientHaiku final : public API::NavigationClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ExtensionBackgroundNavigationClientHaiku);
public:
    explicit ExtensionBackgroundNavigationClientHaiku(WebExtensionContext& context)
        : m_context(context) { }

private:
    RefPtr<WebExtensionContext> contextForPage(WebPageProxy& page) const
    {
        // Closing a page can invoke callbacks while its context is being
        // destroyed. Check cleared identity before taking another reference.
        auto* context = m_context.get();
        if (!context || !context->isLoaded() || context->backgroundWebView() != &page)
            return nullptr;
        return context;
    }

    void decidePolicyForNavigationAction(WebPageProxy& page, Ref<API::NavigationAction>&& action, Ref<WebFramePolicyListenerProxy>&& listener) final
    {
        RefPtr context = contextForPage(page);
        if (context && context->decidePolicyForNavigationAction(&page, action.ptr()))
            listener->use();
        else
            listener->ignore();
    }

    void didFinishDocumentLoad(WebPageProxy& page, API::Navigation*, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->didFinishDocumentLoad(&page);
    }

    void didFailProvisionalNavigationWithError(WebPageProxy& page, FrameInfoData&&, API::Navigation*, const URL&, const WebCore::ResourceError& error, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->didFailNavigation(&page, API::Error::create(error));
    }

    void didFailNavigationWithError(WebPageProxy& page, const FrameInfoData&, API::Navigation*, const URL&, const WebCore::ResourceError& error, API::Object*) final
    {
        if (RefPtr context = contextForPage(page))
            context->didFailNavigation(&page, API::Error::create(error));
    }

    bool processDidTerminate(WebPageProxy& page, ProcessTerminationReason) final
    {
        if (RefPtr context = contextForPage(page))
            context->webViewWebContentProcessDidTerminate(&page);
        return true;
    }

    WeakPtr<WebExtensionContext> m_context;
};

void WebExtensionContext::enumerateExtensionPages(NOESCAPE Function<void(WebPageProxy&, bool&)>&& action)
{
    RefPtr controller = extensionController();
    if (!controller)
        return;

    bool stop = false;
    for (Ref page : controller->allPages()) {
        if (!isURLForThisExtension(page->configuration().requiredWebExtensionBaseURL()))
            continue;
        action(page, stop);
        if (stop)
            return;
    }
}

void WebExtensionContext::updateCORSDisablingPatternsOnAllExtensionPages()
{
    auto patterns = corsDisablingPatterns();
    enumerateExtensionPages([&](auto& page, bool&) {
        page.setCORSDisablingPatterns(Vector<String> { patterns });
    });
}

WebPageProxy* WebExtensionContext::relatedWebView()
{
    ASSERT(isLoaded());
    if (m_backgroundWebView)
        return m_backgroundWebView.get();

    WebPageProxy* result = nullptr;
    enumerateExtensionPages([&](auto& page, bool& stop) {
        result = &page;
        stop = true;
    });
    return result;
}

// Store packages decide which browser they run in from navigator.userAgent, not
// from the API surface. Summit's Safari user agent sends them down their Safari
// Web Extension paths, which assume a native container app: Bitwarden (both its
// Chrome and Firefox builds) then retries runtime.sendNativeMessage("sleep") in
// an unthrottled loop (~7,000 iterations a second) and routes clipboard and
// biometrics to the missing app. Extension-owned documents therefore report the
// user agent of the browser the package was built for: Chrome for a CRX
// identity (32 letters a-p), Firefox for a Gecko identity (contains '@' or is a
// {UUID}). The background service worker inherits it from its registering
// document. Web pages and content scripts keep Summit's normal user agent.
enum class ExtensionPackageFamily : uint8_t { Other, Chrome, Firefox };

static ExtensionPackageFamily packageFamily(const String& identifier)
{
    if (identifier.length() == 32) {
        bool chrome = true;
        for (auto character : StringView { identifier }.codeUnits())
            chrome = chrome && character >= 'a' && character <= 'p';
        if (chrome)
            return ExtensionPackageFamily::Chrome;
    }
    if (identifier.contains('@') || (identifier.startsWith('{') && identifier.endsWith('}')))
        return ExtensionPackageFamily::Firefox;
    return ExtensionPackageFamily::Other;
}

static String extensionPageUserAgent(const String& uniqueIdentifier)
{
    // SUMMIT_EXTENSION_STORE_UA=0 keeps Summit's own user agent everywhere.
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_EXTENSION_STORE_UA");
        return !value || std::strcmp(value, "0");
    }();
    if (!enabled)
        return { };
    auto family = packageFamily(uniqueIdentifier);
    if (family == ExtensionPackageFamily::Other)
        return { };
    // Keep the platform token of the standard user agent, e.g. "(Macintosh; Intel Haiku/x86_64)".
    auto standard = WebCore::standardUserAgent();
    auto start = standard.find('(');
    auto end = standard.find(')');
    String platform = "(X11; Haiku x86_64)"_s;
    if (start != notFound && end != notFound && end > start)
        platform = standard.substring(start, end - start + 1);
    if (family == ExtensionPackageFamily::Chrome)
        return makeString("Mozilla/5.0 "_s, platform, " AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"_s);
    // Firefox puts its rv: token inside the platform parentheses.
    return makeString("Mozilla/5.0 "_s, platform.left(platform.length() - 1), "; rv:140.0) Gecko/20100101 Firefox/140.0"_s);
}

WebViewConfiguration WebExtensionContext::webViewConfiguration(WebViewPurpose purpose)
{
    RefPtr controller = extensionController();
    if (!controller)
        return nullptr;

    Ref configuration = API::PageConfiguration::create();
    configuration->setWebsiteDataStore(Ref { controller->configuration().defaultWebsiteDataStore() });
    configuration->setContentSecurityPolicyModeForExtension(protect(extension())->supportsManifestVersion(3)
        ? WebCore::ContentSecurityPolicyModeForExtension::ManifestV3 : WebCore::ContentSecurityPolicyModeForExtension::ManifestV2);
    configuration->setCORSDisablingPatterns(corsDisablingPatterns());
    configuration->setCrossOriginAccessControlCheckEnabled(false);
    configuration->setProcessDisplayName(processDisplayName());
    configuration->setRequiredWebExtensionBaseURL(URL { baseURL() });
    configuration->setShouldRelaxThirdPartyCookieBlocking(WebCore::ShouldRelaxThirdPartyCookieBlocking::Yes);
    configuration->setMaskedURLSchemes({ });
    configuration->defaultWebsitePolicies().setAutoplayPolicy(WebsiteAutoplayPolicy::Allow);
    if (auto userAgent = extensionPageUserAgent(uniqueIdentifier()); !userAgent.isEmpty())
        configuration->defaultWebsitePolicies().setCustomUserAgent(WTF::move(userAgent));

    Ref preferences = configuration->preferences();
    // Opt-in diagnostics for extension-owned pages; package scripts remain unchanged.
    if (const char* trace = std::getenv("SUMMIT_TRACE_EXTENSION_CONSOLE"); trace && !std::strcmp(trace, "1"))
        preferences->setLogsPageMessagesToSystemConsoleEnabled(true);
    preferences->setAcceleratedCompositingEnabled(false);
    // Extensions written for Chrome and Firefox call requestIdleCallback unconditionally.
    preferences->setRequestIdleCallbackEnabled(true);
    // As for web pages (WebView.cpp): navigator.storage and OffscreenCanvas,
    // which extensions use in their background pages and service workers.
    preferences->setStorageAPIEnabled(true);
    preferences->setStorageAPIEstimateEnabled(true);
#if ENABLE(OFFSCREEN_CANVAS)
    if (const char* offscreen = std::getenv("SUMMIT_OFFSCREEN_CANVAS"); !offscreen || std::strcmp(offscreen, "0")) {
        preferences->setOffscreenCanvasEnabled(true);
        preferences->setOffscreenCanvasInWorkersEnabled(true);
    }
#endif
    preferences->setForceCompositingMode(false);
    preferences->setThreadedScrollingEnabled(false);
    preferences->setJavaScriptCanAccessClipboard(hasPermission(WebExtensionPermission::clipboardWrite()));

    if (RefPtr relatedPage = relatedWebView()) {
        configuration->setProcessPool(Ref { relatedPage->configuration().processPool() });
        if (!preferences->siteIsolationEnabled())
            configuration->setRelatedPage(WeakPtr { *relatedPage });
    }

    if (purpose == WebViewPurpose::Tab)
        configuration->setWebExtensionController(WTF::move(controller));
    else
        configuration->setWeakWebExtensionController(controller.get());

    bool hiddenExtensionDocument = purpose == WebViewPurpose::Background || purpose == WebViewPurpose::Inspector;
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    // Offscreen documents play audio, parse DOM and use timers while no window is visible.
    hiddenExtensionDocument = hiddenExtensionDocument || purpose == WebViewPurpose::Offscreen;
#endif
    if (hiddenExtensionDocument) {
        preferences->setHiddenPageDOMTimerThrottlingEnabled(false);
        preferences->setPageVisibilityBasedProcessSuppressionEnabled(false);
        preferences->setShouldTakeNearSuspendedAssertions(true);
        preferences->setBackgroundWebContentRunningBoardThrottlingEnabled(false);
        preferences->setShouldDropNearSuspendedAssertionAfterDelay(false);
    }
    return configuration;
}

bool WebExtensionContext::isBackgroundPage(WebPageProxyIdentifier identifier) const
{
    return m_backgroundWebView && m_backgroundWebView->identifier() == identifier;
}

std::optional<WebCore::PageIdentifier> WebExtensionContext::backgroundPageIdentifier(WebProcessProxy& destinationProcess) const
{
    if (!m_backgroundWebView || protect(extension())->backgroundContentIsServiceWorker())
        return std::nullopt;
    return m_backgroundWebView->webPageIDInProcess(destinationProcess);
}

std::optional<WebCore::PageIdentifier> WebExtensionContext::backgroundPageIdentifierInOwnProcess() const
{
    if (!m_backgroundWebView || protect(extension())->backgroundContentIsServiceWorker())
        return std::nullopt;
    RefPtr frame = m_backgroundWebView->mainFrame();
    return frame ? frame->webPageIDInCurrentProcess() : std::nullopt;
}

bool WebExtensionContext::backgroundContentIsLoaded() const
{
    return m_backgroundWebView && m_backgroundContentIsLoaded && m_actionsToPerformAfterBackgroundContentLoads.isEmpty();
}

void WebExtensionContext::loadBackgroundWebViewIfNeeded()
{
    ASSERT(isLoaded());
    if (!protect(extension())->hasBackgroundContent() || m_backgroundWebView || !safeToLoadBackgroundContent())
        return;
    loadBackgroundWebView();
}

void WebExtensionContext::loadBackgroundWebView()
{
    ASSERT(isLoaded());
    if (!protect(extension())->hasBackgroundContent() || !extensionController())
        return;

    ASSERT(safeToLoadBackgroundContent());
    ASSERT(!m_backgroundWebView);
    ASSERT(!m_backgroundPageClient);
    ASSERT(!m_backgroundContentIsLoaded);

    // Saved listeners select wake-up events. The new document's registrations
    // replace that cache; counting both would accumulate stale registrations.
    m_backgroundContentEventListeners.clear();
    m_backgroundContentHasLoadedOnce = false;

    RefPtr configuration = webViewConfiguration(WebViewPurpose::Background);
    if (!configuration)
        return;
    SUMMIT_EXTENSION_TIMING("background page of %s created", uniqueIdentifier().utf8().legacyCStringPointer());
    m_backgroundPageClient = WebView::createForExtensionBackground(configuration.releaseNonNull());
    m_backgroundWebView = m_backgroundPageClient->page();
    Ref page = *m_backgroundWebView;
    page->setNavigationClient(makeUniqueRef<ExtensionBackgroundNavigationClientHaiku>(*this));
    page->setInspectable(m_inspectable);
    setBackgroundWebViewInspectionName(backgroundWebViewInspectionName());
    clearError(Error::BackgroundContentFailedToLoad);
    m_backgroundContentLoadError = nullptr;

    constexpr auto activityName = "Web Extension background content"_s;
    m_backgroundWebViewActivity = protect(page->activityGroupContext())->foregroundProcessActivityGroup(activityName);

    if (!protect(extension())->backgroundContentIsServiceWorker()) {
        page->forEachWebContentProcess([&](auto& process, auto pageID) {
            process.send(Messages::WebExtensionContextProxy::SetBackgroundPageIdentifier(pageID), identifier());
        });
        page->loadRequest(WebCore::ResourceRequest { backgroundContentURL() });
        return;
    }

    page->loadServiceWorker(backgroundContentURL(), protect(extension())->backgroundContentUsesModules(),
        [weakThis = WeakPtr { *this }, weakPage = WeakPtr { page.get() }](bool success) {
            if (!weakThis || !weakThis->isLoaded() || !weakPage || weakThis->backgroundWebView() != weakPage.get())
                return;
            Ref context = *weakThis;
            Ref page = *weakPage;
            if (!success) {
                context->didFailNavigation(page.ptr(), nullptr);
                return;
            }
            context->performTasksAfterBackgroundContentLoads();
        });
}

void WebExtensionContext::unloadBackgroundWebView()
{
    cancelBackgroundContentTasks();
    m_backgroundContentIsLoaded = false;
    m_unloadBackgroundWebViewTimer = nullptr;
    m_backgroundWebViewActivity = { };
    // Clear identity before closing: close may complete a pending worker load.
    m_backgroundWebView = nullptr;
    if (RefPtr client = std::exchange(m_backgroundPageClient, nullptr))
        client->close();
}

void WebExtensionContext::setBackgroundWebViewInspectionName(const String& name)
{
    m_backgroundWebViewInspectionName = name;
    if (m_backgroundWebView)
        m_backgroundWebView->setRemoteInspectionNameOverride(name);
}

bool WebExtensionContext::isNotRunningInTestRunner()
{
    RefPtr controller = extensionController();
    return !controller || !controller->inTestingMode();
}

void WebExtensionContext::unloadBackgroundContentIfPossible()
{
    if (!m_backgroundWebView || protect(extension())->backgroundContentIsPersistent())
        return;
    auto delayForInactivePorts = isNotRunningInTestRunner() ? 2_min : 6_s;
    if (m_pendingPermissionRequests
        || (pageHasOpenPorts(*m_backgroundWebView) && MonotonicTime::now() - m_lastBackgroundPortActivityTime < delayForInactivePorts)
        || m_backgroundWebView->hasInspectorFrontend()) {
        scheduleBackgroundContentToUnload();
        return;
    }
#if ENABLE(INSPECTOR_EXTENSIONS)
    if (!m_inspectorContextMap.isEmptyIgnoringNullReferences()) {
        scheduleBackgroundContentToUnload();
        return;
    }
#endif
    unloadBackgroundWebView();
}

bool WebExtensionContext::decidePolicyForNavigationAction(WebPageProxy* page, API::NavigationAction* action)
{
    if (page != m_backgroundWebView || !action)
        return false;
    RefPtr targetFrame = action->targetFrame();
    return (targetFrame && !targetFrame->isMainFrame()) || isURLForThisExtension(action->request().url());
}

void WebExtensionContext::didFinishDocumentLoad(WebPageProxy* page)
{
    if (page != m_backgroundWebView || protect(extension())->backgroundContentIsServiceWorker())
        return;
    SUMMIT_EXTENSION_TIMING("background page of %s loaded", uniqueIdentifier().utf8().legacyCStringPointer());
    performTasksAfterBackgroundContentLoads();
}

void WebExtensionContext::didFailNavigation(WebPageProxy* page, RefPtr<API::Error> error)
{
    if (page != m_backgroundWebView)
        return;
    m_backgroundContentLoadError = createError(Error::BackgroundContentFailedToLoad, nullString(), error);
    recordErrorIfNeeded(backgroundContentLoadError());
    unloadBackgroundWebView();
}

void WebExtensionContext::webViewWebContentProcessDidTerminate(WebPageProxy* page)
{
    if (page != m_backgroundWebView)
        return;
    unloadBackgroundWebView();
    if (isLoaded() && protect(extension())->backgroundContentIsPersistent())
        loadBackgroundWebView();
}

void WebExtensionContext::loadBackgroundPageListenersFromStorage()
{
    if (!storageIsPersistent() || protect(extension())->backgroundContentIsPersistent())
        return;
    m_backgroundContentEventListeners.clear();
    if (auto listeners = readBackgroundListenersHaiku(*m_state)) {
        m_backgroundContentEventListeners = WTF::move(*listeners);
        return;
    }
    // Discard stale or malformed state so initial background loading discovers
    // the current listener set, including events enabled by a new build.
    clearBackgroundListenersHaiku(*m_state);
    writeStateToStorage();
}

void WebExtensionContext::saveBackgroundPageListenersToStorage()
{
    if (!storageIsPersistent() || protect(extension())->backgroundContentIsPersistent())
        return;
    if (saveBackgroundListenersHaiku(*m_state, m_backgroundContentEventListeners))
        writeStateToStorage();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
