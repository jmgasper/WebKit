/*
   Copyright (C) 2014 Haiku, inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#pragma once

#include "APIObject.h"
#include "DefaultUndoController.h"
#include "PageClient.h"
#include "WebKitView.h"
#include "WebViewStateHaiku.h"
#include <wtf/CompletionHandler.h>

namespace API { class PageConfiguration; }

namespace WebKit {

#if USE(COORDINATED_GRAPHICS)
class AcceleratedBackingStore;
#endif

class WebView final : public API::ObjectImpl<API::Object::Type::View>, public PageClient {
    WTF_MAKE_TZONE_ALLOCATED(WebView);
public:
    static Ref<WebView> create(std::shared_ptr<WebViewStateHaiku>);
    // A page the engine asked for (window.open, a link with a target), made
    // with the opener's configuration.
    static Ref<WebView> createWithConfiguration(std::shared_ptr<WebViewStateHaiku>, Ref<API::PageConfiguration>&&);
#if ENABLE(WK_WEB_EXTENSIONS)
    static Ref<WebView> createForExtensionBackground(Ref<API::PageConfiguration>&&);
    static Ref<WebView> createForExtensionPopup(std::shared_ptr<WebViewStateHaiku>, Ref<API::PageConfiguration>&&);
    static Ref<WebView> createForExtensionPage(std::shared_ptr<WebViewStateHaiku>, Ref<API::PageConfiguration>&&);
#endif
    ~WebView() override;
    void ref() const { API::Object::ref(); }
    void deref() const { API::Object::deref(); }
    void close();
    void handleMessage(const BMessage&);
    WebPageProxy* page() const { return m_page.get(); }
    // The attached BWebKitView, or an invalid messenger while it is detached.
    BMessenger nativeView() const
    {
        std::lock_guard guard(m_state->mutex);
        return m_state->view;
    }
    void notifyState(uint32_t = B_WEBKIT_STATE_CHANGED) const;
    // The frame the view shows now (device pixels), or null.
    std::shared_ptr<const BitmapPresenterHaiku::Frame> presentedFrameHaiku() const { return m_state->presenter.snapshot(); }
    // A frame the view draws straight from the web process's shared memory.
    // drawn, if given, runs once the view has drawn the frame (or could not,
    // or after a tenth of a second).
    void presentSharedFrameHaiku(std::shared_ptr<const BBitmap>&&, const WebCore::IntSize&, float, const Vector<WebCore::IntRect, 1>& damage,
        CompletionHandler<void()>&& drawn = { });
    // The web process put a frame into the screen itself (DirectPresentHaiku.h).
    void presentedDirectlyHaiku();

private:
    explicit WebView(std::shared_ptr<WebViewStateHaiku>);
    void initialize(RefPtr<API::PageConfiguration> = nullptr);
    WebViewGeometryHaiku geometry() const;
    void sendToView(BMessage&) const;
    void updateGeometry();
    void didPresent(uint64_t);

    Ref<DrawingAreaProxy> createDrawingAreaProxy(WebProcessProxy&) override;
    void setViewNeedsDisplay(const WebCore::Region&) override;
    void presentBitmapHaiku(Ref<WebCore::ShareableBitmap>&&, const WebCore::IntSize&, float, CompletionHandler<void()>&&) override;
    void requestScroll(const WebCore::FloatPoint&, const WebCore::IntPoint&, WebCore::ScrollIsAnimated, WebCore::InterruptScrollAnimation) override { }
    WebCore::FloatPoint viewScrollPosition() override { return { }; }
    WebCore::IntSize viewSize() override { return geometry().size; }
    bool isViewWindowActive() override { return geometry().active; }
    bool isViewFocused() override { return geometry().focused; }
    bool isActiveViewVisible() override { return geometry().visible; }
    bool isViewInWindow() override { return geometry().attached; }
    void processDidExit() override;
    void didRelaunchProcess() override;
    void pageClosed() override;
    void preferencesDidChange() override { }
    void toolTipChanged(const String&, const String&) override;
    void setCursor(const WebCore::Cursor&) override;
    void setCursorHiddenUntilMouseMoves(bool) override { }
    void registerEditCommand(Ref<WebEditCommandProxy>&&, UndoOrRedo) override;
    void clearAllEditCommands() override;
    bool canUndoRedo(UndoOrRedo) override;
    void executeUndoRedo(UndoOrRedo) override;
    void wheelEventWasNotHandledByWebCore(const NativeWebWheelEvent&) override { }
    WebCore::FloatRect convertToDeviceSpace(const WebCore::FloatRect& rect) override { return rect; }
    WebCore::FloatRect convertToUserSpace(const WebCore::FloatRect& rect) override { return rect; }
    WebCore::IntPoint screenToRootView(const WebCore::IntPoint&) override;
    WebCore::IntPoint rootViewToScreen(const WebCore::IntPoint&) override;
    WebCore::IntRect rootViewToScreen(const WebCore::IntRect&) override;
    WebCore::IntPoint accessibilityScreenToRootView(const WebCore::IntPoint& point) override { return screenToRootView(point); }
    WebCore::IntRect rootViewToAccessibilityScreen(const WebCore::IntRect& rect) override { return rootViewToScreen(rect); }
    void doneWithKeyEvent(const NativeWebKeyboardEvent&, bool) override { }
    RefPtr<WebPopupMenuProxy> createPopupMenuProxy(WebPageProxy&) override;
#if ENABLE(CONTEXT_MENUS)
    Ref<WebContextMenuProxy> createContextMenuProxy(WebPageProxy&, FrameInfoData&&, ContextMenuContextData&&, const UserData&) override;
#endif
    RefPtr<WebColorPicker> createColorPicker(WebPageProxy&, const WebCore::Color&, const WebCore::IntRect&, ColorControlSupportsAlpha, Vector<WebCore::Color>&&, std::optional<WebCore::FrameIdentifier>) override;
    RefPtr<WebDataListSuggestionsDropdown> createDataListSuggestionsDropdown(WebPageProxy&) override;
    RefPtr<WebDateTimePicker> createDateTimePicker(WebPageProxy&) override;
#if USE(COORDINATED_GRAPHICS)
    // The layer tree context carries the identifier of the surface the web
    // process paints into; the backing store receives its frames.
    void enterAcceleratedCompositingMode(const LayerTreeContext& context) override { updateAcceleratedSurface(context.contextID); }
    void exitAcceleratedCompositingMode() override { updateAcceleratedSurface(0); }
    void updateAcceleratedCompositingMode(const LayerTreeContext& context) override { updateAcceleratedSurface(context.contextID); }
    void updateAcceleratedSurface(uint64_t);
#else
    void enterAcceleratedCompositingMode(const LayerTreeContext&) override { }
    void exitAcceleratedCompositingMode() override { }
    void updateAcceleratedCompositingMode(const LayerTreeContext&) override { }
#endif
    void didChangeContentSize(const WebCore::IntSize&) override;
    void didCommitLoadForMainFrame(const String&, bool) override { notifyState(); }
    void didFinishLoadingDataForCustomContentProvider(const String&, std::span<const uint8_t>) override { }
    void navigationGestureDidBegin() override { }
    void navigationGestureWillEnd(bool, WebBackForwardListItem&) override { }
    void navigationGestureDidEnd(bool, WebBackForwardListItem&) override { }
    void navigationGestureDidEnd() override { }
    void willRecordNavigationSnapshot(WebBackForwardListItem&) override { }
    void didRemoveNavigationGestureSnapshot() override { }
    void didFirstVisuallyNonEmptyLayoutForMainFrame() override { notifyState(); }
    void didFinishNavigation(API::Navigation*) override { notifyState(); }
    void didFailNavigation(API::Navigation*) override { notifyState(); }
    void didSameDocumentNavigationForMainFrame(SameDocumentNavigationType) override { notifyState(); }
    void didChangeBackgroundColor() override { }
    void isPlayingAudioWillChange() override { }
    void isPlayingAudioDidChange() override { notifyState(); }
    // Camera, microphone or screen capture started or stopped: the host shows it.
    void microphoneCaptureChanged() override { notifyState(); }
    void cameraCaptureChanged() override { notifyState(); }
    void displayCaptureChanged() override { notifyState(); }
    void refView() override { ref(); }
    void derefView() override { deref(); }
    void didRestoreScrollPosition() override { }
    WebCore::UserInterfaceLayoutDirection userInterfaceLayoutDirection() override { return WebCore::UserInterfaceLayoutDirection::LTR; }
    void requestDOMPasteAccess(WebCore::DOMPasteAccessCategory, WebCore::DOMPasteRequiresInteraction, WebCore::FrameIdentifier, const WebCore::IntRect&, const String&, CompletionHandler<void(WebCore::DOMPasteAccessResponse)>&&) override;
    void processWillSwap() override;

    const std::shared_ptr<WebViewStateHaiku> m_state;
    RefPtr<WebPageProxy> m_page;
    DefaultUndoController m_undoController;
    CompletionHandler<void()> m_presentationCallback;
    uint64_t m_presentationIdentifier { 0 };
    // A frame the view never showed breaks the chain of damage from one
    // frame to the next, so the next one it shows is redrawn whole.
    bool m_presentationNeedsFullRedraw { true };
    uint32_t m_buttons { 0 };
    // The files of the drag over the view (viewDragHaiku).
    Vector<String> m_dragFiles;

#if USE(COORDINATED_GRAPHICS)
    RefPtr<AcceleratedBackingStore> m_acceleratedBackingStore;
#endif
};

} // namespace WebKit
