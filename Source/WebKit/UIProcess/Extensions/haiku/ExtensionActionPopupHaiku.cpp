/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "ExtensionActionPopupHaiku.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIFrameInfo.h"
#include "APINavigationAction.h"
#include "APINavigationClient.h"
#include "APIPageConfiguration.h"
#include "BrowserTabRegistryHaiku.h"
#include "PageLoadState.h"
#include "WebExtensionAction.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionTab.h"
#include "WebExtensionTimingHaiku.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionWindow.h"
#include "WebFramePolicyListenerProxy.h"
#include "WebKitView.h"
#include "WebPageProxy.h"
#include "WebViewPrivate.h"
#include "WebViewStateHaiku.h"
#include <Application.h>
#include <WebCore/ResourceRequest.h>
#include <wtf/SharedTask.h>

namespace WebKit {

struct ExtensionActionPopupHaiku::MainDelivery {
    // Accessed only on main. Clear before native workers release this box, so
    // neither the callback nor its WeakPtr is ever destroyed on a window thread.
    RefPtr<SharedTask<void(NativeExtensionPopupHaiku::Event, status_t)>> callback;
};

class ExtensionPopupNavigationClientHaiku final : public API::NavigationClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ExtensionPopupNavigationClientHaiku);
public:
    explicit ExtensionPopupNavigationClientHaiku(ExtensionActionPopupHaiku& popup) : m_popup(popup) { }
private:
    RefPtr<ExtensionActionPopupHaiku> popupForPage(WebPageProxy& page) const
    {
        auto* popup = m_popup.get();
        if (!popup || popup->page() != &page)
            return nullptr;
        return popup;
    }
    void decidePolicyForNavigationAction(WebPageProxy& page, Ref<API::NavigationAction>&& action, Ref<WebFramePolicyListenerProxy>&& listener) final
    {
        RefPtr popup = popupForPage(page);
        RefPtr frame = action->targetFrame();
        // A popup's top-level document must remain inside its extension. Child
        // frames retain the extension configuration's normal CSP checks.
        if (popup && popup->isCurrent() && ((frame && !frame->isMainFrame()) || popup->allowsNavigation(action->request().url())))
            listener->use();
        else
            listener->ignore();
    }
    void didFinishDocumentLoad(WebPageProxy& page, API::Navigation*, API::Object*) final
    {
        if (RefPtr popup = popupForPage(page))
            popup->documentLoaded();
    }
    void didFailProvisionalNavigationWithError(WebPageProxy& page, FrameInfoData&& frame, API::Navigation*, const URL&, const WebCore::ResourceError&, API::Object*) final
    {
        if (frame.isMainFrame) {
            if (RefPtr popup = popupForPage(page))
                popup->fail("The extension popup document could not be loaded."_s);
        }
    }
    void didFailNavigationWithError(WebPageProxy& page, const FrameInfoData& frame, API::Navigation*, const URL&, const WebCore::ResourceError&, API::Object*) final
    {
        if (frame.isMainFrame) {
            if (RefPtr popup = popupForPage(page))
                popup->fail("The extension popup navigation failed."_s);
        }
    }
    bool processDidTerminate(WebPageProxy& page, ProcessTerminationReason) final
    {
        if (RefPtr popup = popupForPage(page))
            popup->fail("The extension popup web process terminated."_s);
        return true;
    }
    WeakPtr<ExtensionActionPopupHaiku> m_popup;
};

Ref<ExtensionActionPopupHaiku> ExtensionActionPopupHaiku::create(WebExtensionContext& context, WebExtensionAction& action, WebExtensionTab& tab)
{
    return adoptRef(*new ExtensionActionPopupHaiku(context, action, tab));
}

ExtensionActionPopupHaiku::ExtensionActionPopupHaiku(WebExtensionContext& context, WebExtensionAction& action, WebExtensionTab& tab)
    : BHandler("WebKit extension action popup")
    , m_context(context)
    , m_action(action)
    , m_tab(tab)
    , m_loadIdentifier(context.privilegedIdentifier())
    , m_popupURL(context.baseURL(), action.popupPath())
    , m_ownerURL(tab.nativePage() ? tab.nativePage()->pageLoadState().url() : URL { })
    , m_ownerWindowIdentifier(tab.window() ? tab.window()->nativeWindowIdentifier() : 0)
    , m_delivery(std::make_shared<MainDelivery>())
    , m_timer(RunLoop::mainSingleton(), "ExtensionActionPopupHaiku::Timer"_s, this, &ExtensionActionPopupHaiku::tick)
{
    ASSERT(RunLoop::isMain());
    ASSERT(be_app && be_app->Thread() == find_thread(nullptr));
    be_app->AddHandler(this);
    m_state = std::make_shared<WebViewStateHaiku>(BMessenger(this));
    m_state->geometry.size = { 360, 240 };
    m_state->status = B_OK;
}

ExtensionActionPopupHaiku::~ExtensionActionPopupHaiku()
{
    ASSERT(RunLoop::isMain());
    ASSERT(m_closed && !m_page && !m_state->client && !m_delivery->callback && !m_presentationCompletion);
    if (Looper())
        Looper()->RemoveHandler(this);
}

bool ExtensionActionPopupHaiku::isForTab(const WebExtensionTab& tab) const { return m_tab.ptr() == &tab; }

void ExtensionActionPopupHaiku::setPresentationCompletionHandler(PresentationCompletionHandler&& completion)
{
    ASSERT(RunLoop::isMain());
    ASSERT(!m_closed && !m_shown && !m_presentationCompletion);
    m_presentationCompletion = WTF::move(completion);
}

bool ExtensionActionPopupHaiku::isCurrent() const
{
    ASSERT(RunLoop::isMain());
    auto* context = m_context.get();
    if (m_closed || m_state->closed || !context || !context->isLoaded() || context->privilegedIdentifier() != m_loadIdentifier
        || context->m_nativeActionPopup.get() != this || !context->isValidTab(m_tab) || !m_tab->isOpen() || !m_tab->extensionHasAccess()
        || context->getAction(m_tab.ptr()).ptr() != m_action.ptr() || !m_action->isEnabled() || !m_action->presentsPopup()
        || URL { context->baseURL(), m_action->popupPath() } != m_popupURL)
        return false;
    RefPtr ownerPage = m_tab->nativePage();
    RefPtr window = m_tab->window();
    return ownerPage && !ownerPage->isClosed() && ownerPage->pageLoadState().url() == m_ownerURL
        && window && window->isOpen() && window->nativeWindowIdentifier() == m_ownerWindowIdentifier && window->activeTab().get() == m_tab.ptr()
        && (!m_page || !m_page->isClosed());
}

bool ExtensionActionPopupHaiku::allowsNavigation(const URL& url) const
{
    return isCurrent() && m_context->isURLForThisExtension(url);
}

bool ExtensionActionPopupHaiku::start()
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (!isCurrent() || !allowsNavigation(m_popupURL)) {
        fail("The extension popup has no valid action or tab."_s);
        return false;
    }
    RefPtr context = m_context.get();
    RefPtr controller = context->extensionController();
    RefPtr window = m_tab->window();
    auto nativeWindow = controller ? controller->nativeBrowserTabs()->window(window->nativeWindowIdentifier()) : std::nullopt;
    RefPtr ownerPage = m_tab->nativePage();
    RefPtr pageClient = ownerPage ? ownerPage->pageClient() : nullptr;
    RefPtr configuration = context->webViewConfiguration(WebExtensionContext::WebViewPurpose::Popup);
    SUMMIT_EXTENSION_TIMING("popup start: configuration ready");
    if (!nativeWindow || !nativeWindow->messenger.IsValid() || !pageClient || !configuration) {
        fail("The extension popup has no native browser window."_s);
        return false;
    }
    // Anchor at the browser content's top-right corner, below the toolbar.
    auto size = pageClient->viewSize();
    auto anchor = pageClient->rootViewToScreen(WebCore::IntPoint { std::max(0, size.width() - 1), 0 });
    SUMMIT_EXTENSION_TIMING("popup start: anchor ready");
    m_state->client = WebView::createForExtensionPopup(m_state, configuration.releaseNonNull());
    m_page = m_state->client->page();
    SUMMIT_EXTENSION_TIMING("popup start: page created");
    if (!m_page) {
        fail("The extension popup web page could not be created."_s);
        return false;
    }
    m_page->setNavigationClient(makeUniqueRef<ExtensionPopupNavigationClientHaiku>(*this));
    m_page->setMinimumSizeForAutoLayout({ 25, 25 });
    m_page->setSizeToContentAutoSizeMaximumSize({ 800, 600 });
    m_page->setAutoSizingShouldExpandToViewHeight(false);
    context->addPopupPage(*m_page, m_action);
    if (!m_preloaded) {
        m_action->m_presentsPopupWhenReady = true;
        m_action->m_popupPresented = false;
    }
    m_delivery->callback = createSharedTask<void(NativeExtensionPopupHaiku::Event, status_t)>([weakThis = WeakPtr { *this }](auto event, status_t status) {
        if (RefPtr popup = weakThis.get())
            popup->nativeEvent(event, status);
    });
    m_state->contentSizeChanged = createSharedTask<void(const WebCore::IntSize&)>([weakThis = WeakPtr { *this }](const auto& size) {
        if (RefPtr popup = weakThis.get())
            popup->contentSizeChanged(size);
    });
    auto title = m_action->label().utf8();
    m_native = std::make_unique<NativeExtensionPopupHaiku>(nativeWindow->messenger, BRect(anchor.x(), anchor.y(), anchor.x(), anchor.y()),
        std::string(title.legacyCStringPointer(), title.length()), [state = m_state](BRect frame) -> BView* {
            if (state->closed)
                return nullptr;
            auto view = std::unique_ptr<BWebKitView>(new BWebKitView(frame, "extension-popup-content", state));
            if (view->InitCheck() != B_OK || state->closed)
                return nullptr;
            return view.release();
        }, [delivery = m_delivery](auto event, status_t status) {
            RunLoop::mainSingleton().dispatch([delivery, event, status] {
                if (auto callback = delivery->callback)
                    callback->run(event, status);
            });
        }, [dialogs = m_state->dialogs] { return dialogs->hasPending(); });
    auto status = m_native->start();
    SUMMIT_EXTENSION_TIMING("popup start: native window started");
    if (status != B_OK) {
        fail("The native extension popup window could not be started."_s);
        return false;
    }
    m_startedAt = MonotonicTime::now();
    m_timer.startRepeating(100_ms);
    SUMMIT_EXTENSION_TIMING("popup page created, loading %s%s", m_popupURL.string().utf8().legacyCStringPointer(), m_preloaded ? " (preload)" : "");
    m_page->loadRequest(WebCore::ResourceRequest { URL { m_popupURL } });
    return true;
}

void ExtensionActionPopupHaiku::preload(WebExtensionContext& context, WebExtensionTab& tab)
{
    ASSERT(RunLoop::isMain());
    if (!context.isLoaded() || context.m_nativeActionPopup || !context.isValidTab(tab) || !tab.isOpen() || !tab.extensionHasAccess())
        return;
    Ref action = context.getOrCreateAction(&tab);
    if (!action->isEnabled() || !action->presentsPopup())
        return;
    // Another extension's shown popup stays; a click would supersede it.
    SUMMIT_EXTENSION_TIMING("popup of %s preloading", context.uniqueIdentifier().utf8().legacyCStringPointer());
    Ref popup = ExtensionActionPopupHaiku::create(context, action, tab);
    popup->m_preloaded = true;
    context.m_nativeActionPopup = popup.copyRef();
    popup->start();
}

void ExtensionActionPopupHaiku::present()
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (m_closed || !std::exchange(m_preloaded, false))
        return;
    m_action->m_presentsPopupWhenReady = true;
    m_action->m_popupPresented = false;
    SUMMIT_EXTENSION_TIMING("popup presented from preload, document %s", m_documentLoaded ? "loaded" : "still loading");
    if (m_documentLoaded)
        documentLoaded();
}

void ExtensionActionPopupHaiku::documentLoaded()
{
    Ref protectedThis { *this };
    if (!isCurrent()) {
        close();
        return;
    }
    m_documentLoaded = true;
    if (m_preloaded) {
        SUMMIT_EXTENSION_TIMING("preloaded popup document loaded, waiting for a click");
        return;
    }
    // A delayed programmatic popup must still belong to the focused window
    // when its document is ready. Showing the popup itself transfers focus.
    if (m_presentationCompletion) {
        RefPtr window = m_tab->window();
        if (!window || !window->isFocused()) {
            fail("The popup's browser window is no longer focused."_s);
            return;
        }
    }
    SUMMIT_EXTENSION_TIMING("popup document loaded, showing the window");
    if (m_native)
        m_native->show(!!m_presentationCompletion);
}

void ExtensionActionPopupHaiku::contentSizeChanged(const WebCore::IntSize& reportedSize)
{
    if (!isCurrent() || !m_native || reportedSize.width() <= 0 || reportedSize.height() <= 0)
        return;
    auto size = reportedSize;
    if (size == m_previousContentSize && size != m_lastContentSize)
        m_contentSizeOnlyGrows = true;
    if (m_contentSizeOnlyGrows)
        size = size.expandedTo(m_lastContentSize);
    if (const char* trace = std::getenv("SUMMIT_TRACE_EXTENSION_CONSOLE"); trace && !std::strcmp(trace, "1"))
        std::fprintf(stderr, "Summit extension popup: content %dx%d, window %dx%d%s\n", reportedSize.width(), reportedSize.height(),
            size.width(), size.height(), m_contentSizeOnlyGrows ? " (grow only)" : "");
    SUMMIT_EXTENSION_TIMING("popup content %dx%d", reportedSize.width(), reportedSize.height());
    if (size == m_lastContentSize)
        return;
    m_previousContentSize = std::exchange(m_lastContentSize, size);
    m_native->resize(BSize(size.width(), size.height()));
}

void ExtensionActionPopupHaiku::nativeEvent(NativeExtensionPopupHaiku::Event event, status_t status)
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (m_closed)
        return;
    if (event == NativeExtensionPopupHaiku::Event::Closed) {
        if (status != B_OK && status != B_CANCELED)
            fail("The native extension popup window could not be created."_s);
        else
            close();
        return;
    }
    if (!isCurrent()) {
        close();
        return;
    }
    if (event == NativeExtensionPopupHaiku::Event::Shown) {
        SUMMIT_EXTENSION_TIMING("popup window shown");
        m_shown = true;
        m_action->m_presentsPopupWhenReady = false;
        m_action->m_popupPresented = true;
        m_action->setHasUnreadBadgeText(false);
        if (auto completion = std::exchange(m_presentationCompletion, { }))
            completion({ });
    }
}

void ExtensionActionPopupHaiku::validate()
{
    Ref protectedThis { *this };
    if (!isCurrent())
        close();
}

void ExtensionActionPopupHaiku::tick()
{
    Ref protectedThis { *this };
    if (!isCurrent()) {
        close();
        return;
    }
    if (m_preloaded) {
        // A popup nobody clicked is let go; hovering again makes a fresh one.
        if (MonotonicTime::now() - m_startedAt >= 30_s)
            close();
        return;
    }
    if (!m_shown && MonotonicTime::now() - m_startedAt >= 30_s)
        fail("The extension popup did not finish loading before its deadline."_s);
}

void ExtensionActionPopupHaiku::MessageReceived(BMessage* message)
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (message->what == B_WEBKIT_CLOSE_REQUESTED || message->what == B_WEBKIT_PROCESS_EXITED) {
        BMessenger view;
        {
            std::lock_guard guard(m_state->mutex);
            view = m_state->view;
        }
        BMessenger messageView;
        if (view.IsValid() && message->FindMessenger("view", &messageView) == B_OK && messageView == view)
            close();
        return;
    }
    BHandler::MessageReceived(message);
}

void ExtensionActionPopupHaiku::fail(const String& message)
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (m_closed)
        return;
    m_presentationError = message;
    if (auto* context = m_context.get(); context && context->isLoaded() && context->privilegedIdentifier() == m_loadIdentifier)
        context->recordErrorIfNeeded(context->createError(WebExtensionContext::Error::Unknown, message));
    close();
}

void ExtensionActionPopupHaiku::close()
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (std::exchange(m_closed, true))
        return;
    m_timer.stop();
    m_delivery->callback = nullptr;
    m_state->contentSizeChanged = nullptr;
    m_state->closed = true;
    m_action->m_presentsPopupWhenReady = false;
    m_action->m_popupPresented = false;
    if (auto* context = m_context.get()) {
        if (m_page)
            context->m_popupPageActionMap.remove(*m_page);
        if (context->m_nativeActionPopup.get() == this)
            context->m_nativeActionPopup = nullptr;
    }
    m_context = nullptr;
    // Clear identity before closing a page can invoke its navigation client.
    m_page = nullptr;
    if (auto client = std::exchange(m_state->client, nullptr))
        client->close();
    m_state->dialogs->cancelAll(true);
    m_state->presenter.close();
    if (m_native)
        m_native->cancel();
    // Clear all host identities before delivering the response. A callback
    // cannot leave a pending promise or retain this closed popup's page.
    if (auto completion = std::exchange(m_presentationCompletion, { }))
        completion(toWebExtensionError("action.openPopup()"_s, nullString(), m_presentationError.isEmpty()
            ? "The extension popup closed before it was shown."_s : m_presentationError));
}

void WebExtensionContext::actionOpenPopup(WebPageProxyIdentifier, std::optional<WebExtensionWindowIdentifier> windowIdentifier,
    std::optional<WebExtensionTabIdentifier> tabIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    ASSERT(RunLoop::isMain());
    constexpr auto api = "action.openPopup()"_s;
    auto reject = [&](const String& message) { completion(toWebExtensionError(api, nullString(), message)); };
    if ((windowIdentifier && tabIdentifier)
        || (windowIdentifier && (!isValid(windowIdentifier) || (!isCurrent(windowIdentifier) && windowIdentifier->toUInt64() > 9007199254740991ULL)))
        || (tabIdentifier && (!isValid(tabIdentifier) || tabIdentifier->toUInt64() > 9007199254740991ULL))) {
        reject("Invalid popup target."_s);
        return;
    }
    RefPtr controller = extensionController();
    if (!isLoaded() || !controller) {
        reject("The extension is no longer loaded."_s);
        return;
    }
    RefPtr<WebExtensionTab> tab;
    RefPtr<WebExtensionWindow> window;
    if (tabIdentifier) {
        tab = getTab(*tabIdentifier);
        window = tab ? tab->window() : nullptr;
    } else {
        window = windowIdentifier && !isCurrent(windowIdentifier) ? getWindow(*windowIdentifier) : focusedWindow();
        tab = window ? window->activeTab() : nullptr;
    }
    if (!window || !window->isOpen() || !window->extensionHasAccess() || !window->isFocused()) {
        reject("The target browser window is not focused or accessible."_s);
        return;
    }
    if (!tab || !isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess() || window->activeTab().get() != tab.get()) {
        reject("The popup target is not the active tab."_s);
        return;
    }
    for (auto& context : controller->extensionContexts()) {
        if (RefPtr popup = context->m_nativeActionPopup; popup && popup->isPreloaded())
            popup->close();
        if (context->m_nativeActionPopup) {
            reject("Another extension popup is already open."_s);
            return;
        }
    }
    Ref action = getOrCreateAction(tab.get());
    if (!action->isEnabled() || !action->presentsPopup()) {
        reject("The action is disabled or has no popup."_s);
        return;
    }
    // Programmatic presentation neither grants activeTab nor fires onClicked.
    Ref popup = ExtensionActionPopupHaiku::create(*this, action, *tab);
    popup->setPresentationCompletionHandler(WTF::move(completion));
    m_nativeActionPopup = popup.copyRef();
    popup->start();
}

void WebExtensionContext::closeNativeActionPopup()
{
    if (auto popup = std::exchange(m_nativeActionPopup, nullptr))
        popup->close();
}

void WebExtensionContext::closeNativeActionPopupForTab(WebExtensionTab& tab)
{
    if (m_nativeActionPopup && m_nativeActionPopup->isForTab(tab))
        closeNativeActionPopup();
}

void WebExtensionContext::validateNativeActionPopup()
{
    if (RefPtr popup = m_nativeActionPopup)
        popup->validate();
}

void WebExtensionContext::performAction(WebExtensionTab* tab, UserTriggered userTriggered)
{
    ASSERT(RunLoop::isMain());
    Ref protectedThis { *this };
    if (!isLoaded() || !(extension().hasAction() || extension().hasBrowserAction() || extension().hasPageAction()))
        return;
    RefPtr target = tab;
    if (!target) {
        if (RefPtr window = frontmostWindow(IgnoreExtensionAccess::Yes))
            target = window->activeTab();
    }
    if (!target || !isValidTab(*target) || !target->isOpen() || !target->extensionHasAccess())
        return;
    Ref action = getOrCreateAction(target.get());
    if (!action->isEnabled())
        return;
    auto loadIdentifier = privilegedIdentifier();
    if (userTriggered == UserTriggered::Yes)
        userGesturePerformed(*target);
    // A later action invocation supersedes any pending popup for this browser
    // profile, including another extension's page that has not finished loading.
    RefPtr preloaded = m_nativeActionPopup;
    if (preloaded && !(preloaded->isPreloadedFor(action, *target) && preloaded->isCurrent()))
        preloaded = nullptr;
    Vector<Ref<ExtensionActionPopupHaiku>> previousPopups;
    if (RefPtr controller = extensionController()) {
        for (auto& context : controller->extensionContexts()) {
            if (context->m_nativeActionPopup && context->m_nativeActionPopup != preloaded)
                previousPopups.append(*context->m_nativeActionPopup);
        }
    }
    for (auto& popup : previousPopups)
        popup->close();
    if (preloaded) {
        preloaded->present();
        return;
    }
    closeNativeActionPopup();
    if (!isLoaded() || privilegedIdentifier() != loadIdentifier || !isValidTab(*target) || !target->isOpen()
        || !target->extensionHasAccess() || getAction(target.get()).ptr() != action.ptr() || !action->isEnabled())
        return;
    if (!action->presentsPopup()) {
        fireActionClickedEventIfNeeded(target.get());
        return;
    }
    SUMMIT_EXTENSION_TIMING("popup of %s starting", uniqueIdentifier().utf8().legacyCStringPointer());
    Ref popup = ExtensionActionPopupHaiku::create(*this, action, *target);
    m_nativeActionPopup = popup.copyRef();
    popup->start();
}

} // namespace WebKit
#endif
