/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "NativeExtensionPopupHaiku.h"
#include "WebExtensionContextIdentifier.h"
#include "WebExtensionError.h"
#include <Handler.h>
#include <expected>
#include <wtf/CompletionHandler.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/URL.h>

namespace WebCore { class IntSize; }

namespace WebKit {
class WebExtensionAction;
class WebExtensionContext;
class WebExtensionTab;
class WebPageProxy;
struct WebViewStateHaiku;

// Engine references and BHandler messages belong to the application/main loop.
// The native host retains only shared native state and a main-loop delivery box.
class ExtensionActionPopupHaiku final : public RefCountedAndCanMakeWeakPtr<ExtensionActionPopupHaiku>, public BHandler {
public:
    using PresentationCompletionHandler = CompletionHandler<void(std::expected<void, WebExtensionError>&&)>;
    static Ref<ExtensionActionPopupHaiku> create(WebExtensionContext&, WebExtensionAction&, WebExtensionTab&);
    // Create and load the tab's action popup without showing it, as Firefox
    // does when the pointer reaches the toolbar button, so a click that
    // follows only has to show the window. Nothing happens when the action
    // has no popup, is disabled, or the context already has a popup.
    static void preload(WebExtensionContext&, WebExtensionTab&);
    ~ExtensionActionPopupHaiku() override;
    bool start();
    // Show a preloaded popup now, or as soon as its document has loaded.
    void present();
    bool isPreloaded() const { return m_preloaded; }
    bool isPreloadedFor(const WebExtensionAction& action, const WebExtensionTab& tab) const { return m_preloaded && m_action.ptr() == &action && m_tab.ptr() == &tab; }
    void setPresentationCompletionHandler(PresentationCompletionHandler&&);
    void close();
    void validate();
    bool isForTab(const WebExtensionTab&) const;
    bool isCurrent() const;
    WebPageProxy* page() const { return m_page.get(); }
    bool allowsNavigation(const URL&) const;
    void documentLoaded();
    void fail(const String&);
    void MessageReceived(BMessage*) override;

private:
    ExtensionActionPopupHaiku(WebExtensionContext&, WebExtensionAction&, WebExtensionTab&);
    void nativeEvent(NativeExtensionPopupHaiku::Event, status_t);
    void contentSizeChanged(const WebCore::IntSize&);
    void tick();
    struct MainDelivery;
    WeakPtr<WebExtensionContext> m_context;
    const Ref<WebExtensionAction> m_action;
    const Ref<WebExtensionTab> m_tab;
    const WebExtensionContextIdentifier m_loadIdentifier;
    const URL m_popupURL;
    const URL m_ownerURL;
    const uint64_t m_ownerWindowIdentifier;
    std::shared_ptr<WebViewStateHaiku> m_state;
    std::shared_ptr<MainDelivery> m_delivery;
    std::unique_ptr<NativeExtensionPopupHaiku> m_native;
    RefPtr<WebPageProxy> m_page;
    RunLoop::Timer m_timer;
    MonotonicTime m_startedAt;
    bool m_shown { false };
    bool m_closed { false };
    bool m_preloaded { false };
    bool m_documentLoaded { false };
    // Popups that lay out differently at different window sizes can make size-to-content
    // oscillate. Once a size repeats, the popup only grows.
    WebCore::IntSize m_lastContentSize;
    WebCore::IntSize m_previousContentSize;
    bool m_contentSizeOnlyGrows { false };
    PresentationCompletionHandler m_presentationCompletion;
    String m_presentationError;
};
} // namespace WebKit
#endif
