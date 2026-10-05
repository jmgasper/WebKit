/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebKitView.h"
#include <Path.h>
#include <Entry.h>

#include "WebKit2Initialize.h"
#include "JavaScriptDialogHaiku.h"
#include "NativeSelectPopupHaiku.h"
#include "NativeExtensionPopupHaiku.h"
#include "WebViewPrivate.h"
#include "WebViewStateHaiku.h"
#include "DirectPresentTargetHaiku.h"
#include "PresentStatsHaiku.h"
#include "WebExtensionTimingHaiku.h"
#include <Application.h>
#include <Cursor.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Region.h>
#include <Window.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <utility>

using namespace WebKit;

static std::atomic<bool> webKitInitializedHaiku { false };

static int viewDimensionHaiku(float dimension)
{
    double pixels = std::ceil(static_cast<double>(dimension) + 1);
    return std::isfinite(pixels) && pixels > 0 && pixels <= 8192 ? static_cast<int>(pixels) : 0;
}

// app_server draws a scaled desktop at the density of its monitors and says so
// in the display layout, which only a private libbe call returns. It is looked
// up at run time so the engine still loads on a Haiku without it (scale 1).
static float queryDisplayScaleFactorHaiku()
{
    using GetDisplayLayout = status_t (*)(BMessage&);
    static const auto getDisplayLayout = reinterpret_cast<GetDisplayLayout>(
        dlsym(RTLD_DEFAULT, "_ZN8BPrivate18get_display_layoutER8BMessage"));
    if (!getDisplayLayout)
        return 1;
    BMessage layout;
    if (getDisplayLayout(layout) != B_OK)
        return 1;
    // Every enabled monitor reports the same render scale: the frame buffer
    // has one density, and monitors set larger are enlarged by the card.
    int32 renderScale = 0;
    BMessage display;
    for (int32 index = 0; layout.FindMessage("display", index, &display) == B_OK; ++index) {
        if (!display.GetBool("enabled", false))
            continue;
        int32 scale = display.GetInt32("render scale", 0);
        if (!scale)
            scale = display.GetInt32("scale", 100);
        renderScale = std::max(renderScale, scale);
    }
    if (renderScale < 100 || renderScale > 400)
        return 1;
    return renderScale / 100.0f;
}

float WebKit::displayScaleFactorHaiku()
{
    // SUMMIT_DEVICE_SCALE_FACTOR=1 turns high density off for comparisons.
    static const float forced = [] {
        const char* value = std::getenv("SUMMIT_DEVICE_SCALE_FACTOR");
        float parsed = value ? std::strtof(value, nullptr) : 0;
        return std::isfinite(parsed) && parsed >= 1 && parsed <= 4 ? parsed : 0.0f;
    }();
    if (forced)
        return forced;
    // A geometry update runs on every Draw(); the layout is an app_server round
    // trip, so it is asked again at most once a second.
    static std::atomic<float> cached { 0 };
    static std::atomic<bigtime_t> checkedAt { 0 };
    bigtime_t now = system_time();
    float scale = cached.load(std::memory_order_relaxed);
    if (scale > 0 && now - checkedAt.load(std::memory_order_relaxed) < 1000000)
        return scale;
    scale = queryDisplayScaleFactorHaiku();
    cached.store(scale, std::memory_order_relaxed);
    checkedAt.store(now, std::memory_order_relaxed);
    return scale;
}

status_t BWebKitInitialize()
{
    if (!be_app || be_app->Thread() != find_thread(nullptr))
        return B_BAD_THREAD_ID;
    if (!webKitInitializedHaiku) {
        InitializeWebKit2();
        webKitInitializedHaiku = true;
    }
    return B_OK;
}

float BWebKitDisplayScale()
{
    return displayScaleFactorHaiku();
}

bool BWebKitHasPendingNativeUI()
{
    return JavaScriptDialogHaiku::activeCount() || NativeSelectPopupHaiku::activeCount() || NativeExtensionPopupHaiku::activeCount();
}

// Presenting pages straight into the screen (DirectPresentHaiku.h). A direct
// window's last connection, by window, and the views attached to windows;
// both only under directLockHaiku(). The views keep their frames here.
namespace {
struct DirectWindowHaiku {
    uint32 scale { 0 };
    clipping_rect bounds { };
    std::vector<clipping_rect> clips;
    uint32 frontBytesPerRow { 0 };
    area_id backArea { -1 };
    uint32 backBytesPerRow { 0 };
    uint32 backHeight { 0 };
    // The window's direct daemon, the thread DirectConnected() comes on.
    thread_id daemon { -1 };
};
}

static std::timed_mutex& directLockHaiku()
{
    static NeverDestroyed<std::timed_mutex> lock;
    return lock.get();
}

static std::map<BWindow*, DirectWindowHaiku>& directWindowsHaiku()
{
    static NeverDestroyed<std::map<BWindow*, DirectWindowHaiku>> windows;
    return windows.get();
}

static std::map<WebKit::WebViewStateHaiku*, std::weak_ptr<WebKit::WebViewStateHaiku>>& directViewsHaiku()
{
    static NeverDestroyed<std::map<WebKit::WebViewStateHaiku*, std::weak_ptr<WebKit::WebViewStateHaiku>>> views;
    return views.get();
}

static bool directPresentEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_DIRECT_PRESENT");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

// Where the view's page goes on the screen now, under directLockHaiku().
// Makes the view's target on first use, and asks for the page in full when
// the target changed: the screen holds nothing of it in a newly visible part.
static void updateDirectTargetHaiku(const std::shared_ptr<WebKit::WebViewStateHaiku>& state, bigtime_t deadline)
{
    using WebKit::DirectPresentTargetHaiku;
    DirectPresentTargetHaiku::Target target;
    auto& view = state->directView;
    auto& windows = directWindowsHaiku();
    auto found = view.window ? windows.find(view.window) : windows.end();
    if (found != windows.end() && view.visible && view.frame.IsValid() && found->second.scale >= 100 && !state->closed) {
        const auto& window = found->second;
        // In frame buffer pixels, rounded as app_server's Painter rounds.
        float scale = window.scale / 100.0f;
        float windowLeft = std::round(window.bounds.left / scale);
        float windowTop = std::round(window.bounds.top / scale);
        clipping_rect page;
        page.left = static_cast<int32>(std::floor((windowLeft + view.frame.left) * scale));
        page.top = static_cast<int32>(std::floor((windowTop + view.frame.top) * scale));
        page.right = static_cast<int32>(std::floor((windowLeft + view.frame.right + 1) * scale)) - 1;
        page.bottom = static_cast<int32>(std::floor((windowTop + view.frame.bottom + 1) * scale)) - 1;
        target.enabled = true;
        target.originX = page.left;
        target.originY = page.top;
        target.frontBytesPerRow = window.frontBytesPerRow;
        target.backArea = window.backArea;
        target.backBytesPerRow = window.backBytesPerRow;
        target.backWidth = window.backBytesPerRow / 4;
        target.backHeight = window.backHeight;
        for (auto clip : window.clips) {
            clip.left = std::max(clip.left, page.left);
            clip.top = std::max(clip.top, page.top);
            clip.right = std::min(clip.right, page.right);
            clip.bottom = std::min(clip.bottom, page.bottom);
            if (clip.left <= clip.right && clip.top <= clip.bottom)
                target.rects.push_back(clip);
        }
    }
    // SUMMIT_DIRECT_PRESENT_TRACE=1: each place worked out for a page.
    static const bool trace = [] {
        const char* value = std::getenv("SUMMIT_DIRECT_PRESENT_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    if (trace) {
        std::fprintf(stderr, "Summit direct target: state=%p window=%p connected=%d visible=%d frame=%.0f,%.0f-%.0f,%.0f -> enabled=%d origin=%d,%d rects=%zu",
            state.get(), view.window, found != windows.end(), view.visible, view.frame.left, view.frame.top, view.frame.right, view.frame.bottom,
            target.enabled, static_cast<int>(target.originX), static_cast<int>(target.originY), target.rects.size());
        if (found != windows.end())
            std::fprintf(stderr, " window bounds=%d,%d-%d,%d clips=%zu", static_cast<int>(found->second.bounds.left), static_cast<int>(found->second.bounds.top),
                static_cast<int>(found->second.bounds.right), static_cast<int>(found->second.bounds.bottom), found->second.clips.size());
        std::fprintf(stderr, "\n");
    }
    std::shared_ptr<DirectPresentTargetHaiku> directTarget;
    bool created = false;
    {
        std::lock_guard guard(state->mutex);
        if (!state->directTarget && target.enabled) {
            state->directTarget = DirectPresentTargetHaiku::create();
            created = true;
        }
        directTarget = state->directTarget;
    }
    if (!directTarget)
        return;
    if (created)
        WebKit::dispatchViewMessageHaiku(state, BMessage(WebKit::viewDirectTargetHaiku));
    if (directTarget->update(target, deadline) && target.enabled)
        WebKit::dispatchViewMessageHaiku(state, BMessage(WebKit::viewDirectRedisplayHaiku));
}

void BWebKitView::WindowDirectConnected(BWindow* window, const direct_buffer_info* info)
{
    if (!window || !directPresentEnabledHaiku())
        return;
    // app_server waits half a second for this and then kills the application.
    // Everything here, waiting for the lock included, takes 300 ms at most.
    std::unique_lock guard(directLockHaiku(), std::defer_lock);
    if (!guard.try_lock_for(std::chrono::milliseconds(150))) {
        std::fprintf(stderr, "Summit direct present: the window's views were busy; not updated for this connection\n");
        return;
    }
    bigtime_t deadline = system_time() + 150000;
    auto& windows = directWindowsHaiku();
    // device_scale, drawing_bits_area and drawing_bytes_per_row follow
    // bits_area; in headers from before them they are the start of _reserved1.
    const uint32* extension = info ? reinterpret_cast<const uint32*>(&info->bits_area + 1) : nullptr;
    uint32 scale = extension ? extension[0] : 0;
    if (!info || (info->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_STOP || scale < 100 || info->bits_per_pixel != 32)
        windows.erase(window);
    else {
        DirectWindowHaiku connected;
        connected.daemon = find_thread(nullptr);
        connected.scale = scale;
        connected.bounds = info->window_bounds;
        connected.clips.assign(info->clip_list, info->clip_list + info->clip_list_count);
        connected.frontBytesPerRow = info->bytes_per_row;
        connected.backArea = static_cast<area_id>(extension[1]);
        connected.backBytesPerRow = extension[2];
        area_info backInfo;
        if (connected.backArea < 0 || !connected.backBytesPerRow || get_area_info(connected.backArea, &backInfo) != B_OK) {
            connected.backArea = -1;
            connected.backBytesPerRow = 0;
        } else
            connected.backHeight = backInfo.size / connected.backBytesPerRow;
        windows[window] = std::move(connected);
    }
    for (auto& [pointer, weak] : directViewsHaiku()) {
        auto state = weak.lock();
        if (state && state->directView.window == window)
            updateDirectTargetHaiku(state, deadline);
    }
}

// Window thread, with each frame the web process put into the screen. When
// app_server waits too long for a DirectConnected() it gives the window up and
// its daemon thread ends: the window hears nothing more of being moved,
// covered or of the frame buffer moving (a monitor going to sleep or waking
// does that, and the daemon can wait half a second there on the
// application's app_server link while another thread holds it). Its pages
// then draw through app_server again, at once rather than at the next
// session save (Summit's BrowserWindow checks too).
static void dropWindowsWithoutDaemonHaiku()
{
    static bigtime_t lastCheck = 0;
    bigtime_t now = system_time();
    if (now - lastCheck < 100000)
        return;
    lastCheck = now;
    std::unique_lock guard(directLockHaiku(), std::try_to_lock);
    if (!guard.owns_lock())
        return;
    auto& windows = directWindowsHaiku();
    for (auto it = windows.begin(); it != windows.end();) {
        thread_info info;
        if (it->second.daemon < 0 || get_thread_info(it->second.daemon, &info) == B_OK) {
            ++it;
            continue;
        }
        BWindow* window = it->first;
        it = windows.erase(it);
        std::fprintf(stderr, "Summit direct present: window %p lost its direct daemon; its pages draw through app_server\n", window);
        for (auto& [pointer, weak] : directViewsHaiku()) {
            auto state = weak.lock();
            if (state && state->directView.window == window)
                updateDirectTargetHaiku(state, system_time() + 50000);
        }
    }
}

// Window thread: the view's frame and visibility as the direct window needs them.
static void noteDirectViewHaiku(BWebKitView& view, const std::shared_ptr<WebKit::WebViewStateHaiku>& state, bool attached)
{
    if (!directPresentEnabledHaiku())
        return;
    BWindow* window = attached ? view.Window() : nullptr;
    BRect frame;
    bool visible = false;
    if (window) {
        frame = window->ConvertFromScreen(view.ConvertToScreen(view.Bounds()));
        visible = !view.IsHidden() && !window->IsHidden() && !window->IsMinimized();
    }
    std::lock_guard guard(directLockHaiku());
    auto& views = directViewsHaiku();
    if (window)
        views[state.get()] = state;
    auto& current = state->directView;
    if (current.window == window && current.frame == frame && current.visible == visible) {
        if (!window)
            views.erase(state.get());
        return;
    }
    current.window = window;
    current.frame = frame;
    current.visible = visible;
    updateDirectTargetHaiku(state, system_time() + 90000);
    if (!window)
        views.erase(state.get());
}

BWebKitView::BWebKitView(BRect frame, const char* name, BMessenger target, uint32 resizingMode, std::shared_ptr<BWebKitContext> context)
    : BView(frame, name, resizingMode, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE)
    , m_state(std::make_shared<WebViewStateHaiku>(target))
{
    SetViewColor(B_TRANSPARENT_COLOR);
    if (!webKitInitializedHaiku)
        return;
    if (context) {
        m_status = context->InitCheck();
        m_state->status = m_status;
        if (m_status != B_OK)
            return;
        m_state->context = context->m_context;
    }
    m_status = B_OK;
    m_state->status = B_OK;
    UpdateGeometry();
    WTF::RunLoop::mainSingleton().dispatch([state = m_state] {
        if (!state->closed)
            state->client = WebView::create(state);
    });
}

BWebKitView::BWebKitView(BRect frame, const char* name, BMessenger target, uint64 newPageRequest, uint32 resizingMode, std::shared_ptr<BWebKitContext> context)
    : BView(frame, name, resizingMode, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE)
    , m_state(std::make_shared<WebViewStateHaiku>(target))
{
    SetViewColor(B_TRANSPARENT_COLOR);
    if (!webKitInitializedHaiku)
        return;
    if (context) {
        m_status = context->InitCheck();
        m_state->status = m_status;
        if (m_status != B_OK) {
            DeclineNewPage(newPageRequest);
            return;
        }
        m_state->context = context->m_context;
    }
    m_status = B_OK;
    m_state->status = B_OK;
    UpdateGeometry();
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, newPageRequest] {
        openNewPageHaiku(state, newPageRequest);
    });
}

void BWebKitView::DeclineNewPage(uint64 newPageRequest)
{
    if (!webKitInitializedHaiku)
        return;
    WTF::RunLoop::mainSingleton().dispatch([newPageRequest] {
        declineNewPageHaiku(newPageRequest);
    });
}

void BWebKitView::PerformContextMenuExtensionItem(uint64 token)
{
    if (!webKitInitializedHaiku)
        return;
    WTF::RunLoop::mainSingleton().dispatch([token] {
        performExtensionMenuItemHaiku(token);
    });
}

// Internal extension views adopt a page created with the extension configuration.
// The native window thread owns only this view and the shared native state.
BWebKitView::BWebKitView(BRect frame, const char* name, std::shared_ptr<WebViewStateHaiku> state)
    : BView(frame, name, B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE)
    , m_state(std::move(state))
{
    SetViewColor(B_TRANSPARENT_COLOR);
    m_status = webKitInitializedHaiku && !m_state->closed ? m_state->status.load() : B_NO_INIT;
    m_state->status = m_status;
    if (m_status == B_OK)
        UpdateGeometry();
}

BWebKitView::~BWebKitView()
{
    m_state->closed = true;
    noteDirectViewHaiku(*this, m_state, false);
    m_state->dialogs->cancelAll(true);
    m_dialogHost.reset();
    m_state->presenter.close();
    if (m_status == B_OK) {
        WTF::RunLoop::mainSingleton().dispatch([state = std::move(m_state)] {
            if (auto client = std::exchange(state->client, nullptr))
                client->close();
        });
    }
}

status_t BWebKitView::InitCheck() const { return m_state->status; }

void BWebKitView::SendCommand(uint32 what, const char* url)
{
    if (m_status != B_OK)
        return;
    if (what == viewLoadHaiku || what == viewBackHaiku || what == viewForwardHaiku || what == viewReloadHaiku)
        ++m_state->findRequestIdentifier;
    BMessage message(what);
    if (url)
        message.AddString("url", url);
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::LoadURL(const char* url) { SendCommand(viewLoadHaiku, url); }
void BWebKitView::DownloadURL(const char* url) { SendCommand(viewDownloadHaiku, url); }
void BWebKitView::Preconnect(const char* url) { SendCommand(viewPreconnectHaiku, url); }
void BWebKitView::GoBack() { SendCommand(viewBackHaiku); }
void BWebKitView::GoForward() { SendCommand(viewForwardHaiku); }
void BWebKitView::Reload() { SendCommand(viewReloadHaiku); }
void BWebKitView::Stop() { SendCommand(viewStopHaiku); }

void BWebKitView::RequestClose()
{
    if (m_status == B_OK) {
        SendCommand(viewRequestCloseHaiku);
        return;
    }
    // Initialization can fail before there is a WebKit client or main loop.
    // Such a view has no document whose unload handlers need to run.
    if (m_failedViewCloseNotified)
        return;
    BMessage message(B_WEBKIT_CLOSE_REQUESTED);
    message.AddMessenger("view", BMessenger(this));
    auto result = m_state->target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    m_failedViewCloseNotified = result == B_OK;
    if (result == B_WOULD_BLOCK || result == B_TIMED_OUT) {
        // No WebKit client was created. Retry through the view's native looper;
        // the owned messenger becomes invalid if the view is destroyed.
        BMessage retry(viewRetryCloseHaiku);
        BMessageRunner::StartSending(BMessenger(this), &retry, 10000, 1);
    }
}

void BWebKitView::ResetCloseRequest()
{
    if (m_status == B_OK)
        SendCommand(viewResetCloseHaiku);
    else
        m_failedViewCloseNotified = false;
}

void BWebKitView::CommitClose(const std::vector<BWebKitView*>& views, BMessenger target, uint64 identifier)
{
    if (!webKitInitializedHaiku) {
        // The native-only failure path has no engine documents or WTF loop.
        bool approved = target.IsValid();
        for (auto* view : views)
            approved &= view && !view->m_state->closed && view->m_failedViewCloseNotified;
        if (approved) {
            for (auto* view : views)
                view->m_state->closed = true;
        }
        BMessage result(B_WEBKIT_CLOSE_COMMITTED);
        result.AddUInt64("identifier", identifier);
        result.AddBool("closed", approved);
        auto status = target.SendMessage(&result, static_cast<BHandler*>(nullptr), 0);
        if (status == B_WOULD_BLOCK || status == B_TIMED_OUT) {
            // A detached one-shot uses the registrar's MessageDeliverer, which
            // retains the final message while the destination port is full.
            BMessageRunner::StartSending(target, &result, 10000, 1);
        }
        return;
    }
    std::vector<std::shared_ptr<WebViewStateHaiku>> states;
    states.reserve(views.size());
    for (auto* view : views)
        states.push_back(view ? view->m_state : nullptr);
    RunLoop::mainSingleton().dispatch([states = std::move(states), target, identifier]() mutable {
        commitViewCloseHaiku(std::move(states), target, identifier);
    });
}

void BWebKitView::FindString(const char* query, bool forward, bool caseSensitive, bool wrapSelection)
{
    if (m_status != B_OK)
        return;
    if (!query || !*query) {
        HideFindUI();
        return;
    }
    BMessage message(viewFindHaiku);
    message.AddString("query", query);
    message.AddBool("forward", forward);
    message.AddBool("caseSensitive", caseSensitive);
    message.AddBool("wrapSelection", wrapSelection);
    message.AddUInt64("identifier", ++m_state->findRequestIdentifier);
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::HideFindUI()
{
    ++m_state->findRequestIdentifier;
    SendCommand(viewHideFindHaiku);
}

void BWebKitView::ChangeZoomFactor(int32 direction, bool textOnly)
{
    if (m_status != B_OK)
        return;
    BMessage message(viewZoomHaiku);
    message.AddInt32("direction", direction);
    message.AddBool("textOnly", textOnly);
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::IncreaseZoomFactor(bool textOnly) { ChangeZoomFactor(1, textOnly); }
void BWebKitView::DecreaseZoomFactor(bool textOnly) { ChangeZoomFactor(-1, textOnly); }
void BWebKitView::ResetZoomFactor() { ChangeZoomFactor(0, false); }

void BWebKitView::ExecuteEditCommand(uint32 command)
{
    if (m_status != B_OK)
        return;
    switch (command) {
    case B_CUT: case B_COPY: case B_PASTE: case B_SELECT_ALL: case B_UNDO: case B_REDO:
        break;
    default:
        return;
    }
    BMessage message(viewEditHaiku);
    message.AddUInt32("command", command);
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::UpdateGeometry()
{
    WebViewGeometryHaiku current;
    auto bounds = Bounds();
    current.size = { viewDimensionHaiku(bounds.Width()), viewDimensionHaiku(bounds.Height()) };
    current.attached = Window();
    current.focused = IsFocus();
    current.scale = displayScaleFactorHaiku();
    BMessenger messenger;
    if (auto* window = Window()) {
        BPoint origin = ConvertToScreen(BPoint());
        current.screenOrigin = { static_cast<int>(std::round(origin.x)), static_cast<int>(std::round(origin.y)) };
        BRect windowFrame = window->Frame();
        current.windowFrame = {
            { static_cast<int>(std::round(windowFrame.left)), static_cast<int>(std::round(windowFrame.top)) },
            { viewDimensionHaiku(windowFrame.Width()), viewDimensionHaiku(windowFrame.Height()) }
        };
        current.visible = !IsHidden() && !window->IsHidden();
        current.active = window->IsActive();
        messenger = BMessenger(this);
    }
    bool changed;
    {
        std::lock_guard guard(m_state->mutex);
        changed = m_state->geometry != current || m_state->view != messenger;
        m_state->geometry = current;
        m_state->view = messenger;
    }
    if (changed)
        SendCommand(viewGeometryHaiku);
    noteDirectViewHaiku(*this, m_state, current.attached);
    if (m_dialogHost)
        m_dialogHost->pump(Window(), BMessenger(this), current.visible);
}

void BWebKitView::AttachedToWindow()
{
    BView::AttachedToWindow();
    UpdateGeometry();
}

void BWebKitView::DetachedFromWindow()
{
    noteDirectViewHaiku(*this, m_state, false);
    m_state->dialogs->cancelAll();
    m_dialogHost.reset();
    {
        std::lock_guard guard(m_state->mutex);
        m_state->geometry.attached = false;
        m_state->geometry.visible = false;
        m_state->geometry.focused = false;
        m_state->geometry.active = false;
        m_state->view = BMessenger();
    }
    SendCommand(viewGeometryHaiku);
    BView::DetachedFromWindow();
}

void BWebKitView::FrameResized(float width, float height) { BView::FrameResized(width, height); UpdateGeometry(); }
void BWebKitView::FrameMoved(BPoint point) { BView::FrameMoved(point); UpdateGeometry(); }
void BWebKitView::WindowActivated(bool active) { BView::WindowActivated(active); UpdateGeometry(); }
void BWebKitView::MakeFocus(bool focused) { BView::MakeFocus(focused); UpdateGeometry(); }
void BWebKitView::Show() { BView::Show(); UpdateGeometry(); }
void BWebKitView::Hide() { BView::Hide(); UpdateGeometry(); }

static bool presentTraceHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_SWAPCHAIN_TRACE");
        return value && *value && std::strcmp(value, "0");
    }();
    return enabled;
}

// Window thread, from Draw(): app_server ends this update by copying its own
// copy of the screen over the updated part, page and all, and that copy is
// older than the screen wherever frames went in directly. The page is
// presented again in full once app_server has finished (the message is
// handled after the update ends).
static void noteDirectExposureHaiku(BWebKitView& view, const std::shared_ptr<WebKit::WebViewStateHaiku>& state)
{
    if (state->directExposurePending.exchange(true))
        return;
    BLooper* looper = view.Looper();
    if (!looper || looper->PostMessage(WebKit::viewDirectExposedHaiku, &view) != B_OK)
        state->directExposurePending = false;
}

void BWebKitView::Draw(BRect update)
{
    UpdateGeometry();
    bool direct = false;
    if (m_state->presentedDirectly.load()) {
        std::lock_guard guard(m_state->mutex);
        direct = m_state->directTarget && m_state->directTarget->enabled();
    }
    // When the GPU puts the page on the screen itself, the bitmap is older
    // than the screen. It is drawn all the same: app_server asked for this
    // part again because its own copy of the screen holds something else
    // there now (a window that has moved away), and at the end of the update
    // it copies that copy onto the screen and keeps it for screenshots and
    // for the next time it redraws. The page then goes into the screen again
    // in full (noteDirectExposureHaiku()).
    // A frame that goes into the screen directly while this one is drawn may
    // land before it, and then this older one covers it.
    uint32_t directFramesBefore = m_state->directFrames.load();
    auto frame = m_state->presenter.snapshot();
    if (presentTraceHaiku())
        std::fprintf(stderr, "Summit view %p draw [%.0f,%.0f %.0fx%.0f] frame=%llu view=%dx%d hidden=%d\n", this, update.left, update.top,
            update.Width() + 1, update.Height() + 1, frame ? static_cast<unsigned long long>(frame->identifier) : 0ULL,
            frame ? frame->viewSize.width() : 0, frame ? frame->viewSize.height() : 0, IsHidden() ? 1 : 0);
    PushState();
    SetDrawingMode(B_OP_COPY);
    BRect destination;
    if (frame)
        destination = BRect(0, 0, frame->viewSize.width() - 1, frame->viewSize.height() - 1);
    // The frame is opaque, so only what it does not cover needs the base colour.
    // A window that is being enlarged shows it until the next frame arrives.
    if (!frame || !destination.Contains(update)) {
        SetHighColor(255, 255, 255, 255);
        FillRect(update);
    }
    if (frame) {
        // Copy only the invalidated part. app_server charges per pixel, and a
        // scroll or a small animation touches much less than the whole window.
        BRect visible = update & destination;
        if (visible.IsValid()) {
            float scale = frame->scale > 0 ? frame->scale : 1;
            BRect source(visible.left * scale, visible.top * scale,
                (visible.right + 1) * scale - 1, (visible.bottom + 1) * scale - 1);
            BRect bounds = frame->image->Bounds();
            source = source & bounds;
            if (source.IsValid())
                DrawBitmap(frame->image.get(), source, visible);
        }
    }
    PopState();
    if (direct) {
        noteDirectExposureHaiku(*this, m_state);
        return;
    }
    bigtime_t drawStart = PresentStageStatsHaiku::enabled() ? system_time() : 0;
    Sync();
    if (m_state->directFrames.load() != directFramesBefore)
        noteDirectExposureHaiku(*this, m_state);
    if (drawStart && frame) {
        static PresentStageStatsHaiku draw("draw (app_server)");
        BRect drawn = update & destination;
        float scale = frame->scale > 0 ? frame->scale : 1;
        draw.add(system_time() - drawStart, drawn.IsValid() ? (drawn.Width() + 1) * (drawn.Height() + 1) * scale * scale / 1e6 : 0);
    }
    if (frame && m_status == B_OK) {
        if (webExtensionTimingEnabledHaiku() && Name() && !std::strcmp(Name(), "extension-popup-content"))
            SUMMIT_EXTENSION_TIMING("popup frame %" PRIu64 " drawn (%dx%d)", static_cast<uint64_t>(frame->identifier), frame->viewSize.width(), frame->viewSize.height());
        BMessage message(viewPresentedHaiku);
        message.AddUInt64("identifier", frame->identifier);
        dispatchViewMessageHaiku(m_state, std::move(message));
    }
}

void BWebKitView::SendInput(const BMessage& event, BPoint position)
{
    if (m_status != B_OK || !Window())
        return;
    UpdateGeometry();
    BMessage message(viewInputHaiku);
    // SUMMIT_INPUT_DELAY_TRACE: when it left the window thread.
    message.AddInt64("summit:sentAt", system_time());
    BMessage native(event);
    int32 modifierState;
    if (native.FindInt32("modifiers", &modifierState) != B_OK)
        native.AddInt32("modifiers", modifiers());
    message.AddMessage("event", &native);
    message.AddPoint("local", position);
    message.AddPoint("screen", ConvertToScreen(position));
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::KeyDown(const char* bytes, int32 length)
{
    if (Window() && Window()->CurrentMessage())
        SendInput(*Window()->CurrentMessage());
    else
        BView::KeyDown(bytes, length);
}

void BWebKitView::KeyUp(const char* bytes, int32 length)
{
    if (Window() && Window()->CurrentMessage())
        SendInput(*Window()->CurrentMessage());
    else
        BView::KeyUp(bytes, length);
}

void BWebKitView::MouseDown(BPoint point)
{
    MakeFocus();
    SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
    if (Window() && Window()->CurrentMessage())
        SendInput(*Window()->CurrentMessage(), point);
}

void BWebKitView::MouseUp(BPoint point)
{
    if (Window() && Window()->CurrentMessage())
        SendInput(*Window()->CurrentMessage(), point);
}

// Files dragged from Tracker (any drag carrying refs) are offered to the page
// as an HTML drag; the page sees no mouse moves meanwhile, as in other ports.
void BWebKitView::MouseMoved(BPoint point, uint32 transit, const BMessage* dragMessage)
{
    if (dragMessage && dragMessage->HasRef("refs")) {
        if (transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW) {
            if (m_state->dragActive)
                SendDrag(*dragMessage, point, "exited");
        } else
            SendDrag(*dragMessage, point, m_state->dragActive ? "updated" : "entered");
        return;
    }
    if (m_state->dragActive) {
        // The drag ended somewhere else, or was cancelled.
        SendDrag(BMessage(), point, "exited");
    }
    if (Window() && Window()->CurrentMessage())
        SendInput(*Window()->CurrentMessage(), point);
}

void BWebKitView::SendDrag(const BMessage& drag, BPoint point, const char* phase)
{
    if (m_status != B_OK || !Window())
        return;
    const bool entering = !strcmp(phase, "entered") || !strcmp(phase, "drop");
    m_state->dragActive = !strcmp(phase, "entered") || !strcmp(phase, "updated");
    UpdateGeometry();
    BMessage message(viewDragHaiku);
    message.AddString("phase", phase);
    message.AddPoint("local", point);
    message.AddPoint("screen", ConvertToScreen(point));
    if (entering) {
        entry_ref ref;
        for (int32 index = 0; drag.FindRef("refs", index, &ref) == B_OK; ++index) {
            BEntry entry(&ref, true);
            BPath path;
            if (entry.GetPath(&path) == B_OK)
                message.AddString("path", path.Path());
        }
    }
    dispatchViewMessageHaiku(m_state, std::move(message));
}

void BWebKitView::MessageReceived(BMessage* message)
{
    if (message->WasDropped() && message->HasRef("refs")) {
        SendDrag(*message, ConvertFromScreen(message->DropPoint()), "drop");
        return;
    }
    switch (message->what) {
    case viewRetryCloseHaiku:
        RequestClose();
        return;
    case javaScriptDialogShowHaiku:
        if (!m_dialogHost)
            m_dialogHost = std::make_unique<JavaScriptDialogHostHaiku>(m_state->dialogs);
        m_dialogHost->show(*message, Window(), BMessenger(this), Window() && !Window()->IsHidden() && !IsHidden());
        return;
    case javaScriptDialogClosedHaiku: {
        uint64 identifier;
        if (m_dialogHost && message->FindUInt64("identifier", &identifier) == B_OK)
            m_dialogHost->didClose(identifier, Window(), BMessenger(this), Window() && !Window()->IsHidden() && !IsHidden());
        return;
    }
    case javaScriptDialogsCancelHaiku:
        if (m_dialogHost)
            m_dialogHost->cancelAll();
        return;
    case B_CUT: case B_COPY: case B_PASTE: case B_SELECT_ALL: case B_UNDO: case B_REDO:
        ExecuteEditCommand(message->what);
        return;
    case WebKit::viewDirectExposedHaiku: {
        m_state->directExposurePending = false;
        // app_server handles the end of the update before this.
        Sync();
        std::shared_ptr<WebKit::DirectPresentTargetHaiku> target;
        {
            std::lock_guard guard(m_state->mutex);
            target = m_state->directTarget;
        }
        if (target && target->enabled()) {
            target->invalidate();
            dispatchViewMessageHaiku(m_state, BMessage(WebKit::viewDirectRedisplayHaiku));
        }
        return;
    }
    case viewFrameHaiku: {
        if (presentTraceHaiku())
            std::fprintf(stderr, "Summit view %p frame message damage=%d window=%p hidden=%d\n", this,
                static_cast<int>(message->CountNames(B_RECT_TYPE)), Window(), IsHidden() ? 1 : 0);
        // The web process put this frame into the screen itself.
        if (message->GetBool("direct", false)) {
            dropWindowsWithoutDaemonHaiku();
            return;
        }
        // A frame names what changed; without that it replaces everything.
        BRect damage;
        if (message->FindRect("damage", &damage) != B_OK) {
            Invalidate();
            return;
        }
        for (int32 index = 0; message->FindRect("damage", index, &damage) == B_OK; ++index)
            Invalidate(damage);
        return;
    }
    case viewTooltipHaiku:
        SetToolTip(message->FindString("text"));
        return;
    case viewCursorHaiku: {
        BMessage archive;
        if (message->FindMessage("cursor", &archive) == B_OK) {
            auto cursor = std::make_unique<BCursor>(&archive);
            if (cursor->InitCheck() == B_OK) {
                m_cursor = std::move(cursor);
                SetViewCursor(m_cursor.get());
            }
        }
        return;
    }
    case B_MOUSE_WHEEL_CHANGED: {
        // app_server applies a wheel notch wherever the pointer is, so that is
        // where the page receives it. A caller that synthesizes the message can
        // name the point instead, which is how scrolling is measured without
        // taking the pointer away from whoever is using the machine.
        BPoint point;
        uint32 buttons;
        if (message->FindPoint("summit:view_where", &point) != B_OK)
            GetMouse(&point, &buttons, false);
        SendInput(*message, point);
        return;
    }
    case B_UNMAPPED_KEY_DOWN:
    case B_UNMAPPED_KEY_UP:
        SendInput(*message);
        return;
    default:
        BView::MessageReceived(message);
    }
}
