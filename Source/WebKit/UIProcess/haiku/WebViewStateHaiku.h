/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "BitmapPresenterHaiku.h"
#include "JavaScriptDialogRequestsHaiku.h"
#include <Messenger.h>
#include <Rect.h>
#include <WebCore/IntPoint.h>
#include <WebCore/NavigationIdentifier.h>
#include <WebCore/ScriptExecutionContextIdentifier.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <wtf/RefPtr.h>
#include <wtf/SharedTask.h>
#include <wtf/text/WTFString.h>

class BWindow;

namespace WebKit {

class DirectPresentTargetHaiku;
class WebView;
class WebViewContextHaiku;

enum : uint32_t {
    viewFrameHaiku = 'wvfr', viewGeometryHaiku = 'wvge', viewInputHaiku = 'wvin',
    viewDownloadHaiku = 'wvdn', viewLoadHaiku = 'wvld', viewBackHaiku = 'wvba', viewForwardHaiku = 'wvfo',
    viewReloadHaiku = 'wvrl', viewStopHaiku = 'wvst', viewPresentedHaiku = 'wvpr',
    viewCursorHaiku = 'wvcu', viewTooltipHaiku = 'wvtt',
    viewFindHaiku = 'wvfi', viewHideFindHaiku = 'wvhf',
    viewZoomHaiku = 'wvzo', viewEditHaiku = 'wved', viewRequestCloseHaiku = 'wvcl',
    viewResetCloseHaiku = 'wvcr', viewRetryCloseHaiku = 'wvrc',
    // Files dragged over the view: phase ("entered", "updated", "exited",
    // "drop"), local, screen, repeated path (with entered and drop).
    viewDragHaiku = 'wvdg',
    // Presenting straight into the screen (DirectPresentHaiku.h): the view
    // has a target for its page, or the page needs presenting again in full.
    viewDirectTargetHaiku = 'wvdt', viewDirectRedisplayHaiku = 'wvdr',
    // To the view itself: app_server has copied its own copy of the screen
    // over part of the page (BWebKitView::Draw); once it is done, present the
    // page again in full.
    viewDirectExposedHaiku = 'wvde',
    viewPreconnectHaiku = 'wvpc'
};

struct WebViewGeometryHaiku {
    WebCore::IntSize size;
    WebCore::IntPoint screenOrigin;
    WebCore::IntRect windowFrame;
    // Frame buffer pixels per logical pixel; see displayScaleFactorHaiku().
    float scale { 1 };
    bool attached { false };
    bool visible { false };
    bool focused { false };
    bool active { false };
    bool operator==(const WebViewGeometryHaiku&) const = default;
};

struct WebViewStateHaiku {
    explicit WebViewStateHaiku(BMessenger);
    ~WebViewStateHaiku();

    std::mutex mutex;
    WebViewGeometryHaiku geometry;
    BMessenger view;
    const BMessenger target;
    BitmapPresenterHaiku presenter;
    std::atomic<bool> closed { false };
    // A file drag from another application is over the view (window thread).
    bool dragActive { false };
    std::atomic<status_t> status { B_NO_INIT };
    std::atomic<uint64_t> findRequestIdentifier { 0 };
    const std::shared_ptr<JavaScriptDialogRequestsHaiku> dialogs { std::make_shared<JavaScriptDialogRequestsHaiku>() };
    std::shared_ptr<WebViewContextHaiku> context;
    // Where on the screen the page goes when the view's window is a direct
    // window with B_DIRECT_DEVICE_PIXELS (WebKitView.cpp keeps it current).
    // Made once, on first use; guarded by `mutex`.
    std::shared_ptr<DirectPresentTargetHaiku> directTarget;
    // The view's frame in its window's coordinates and whether it shows,
    // kept by the view for that; guarded by WebKitView.cpp's registry lock.
    struct DirectView {
        BWindow* window { nullptr };
        BRect frame;
        bool visible { false };
    } directView;
    // The last frame went into the screen directly, so the view's bitmap is
    // older than the screen and must not be drawn over it.
    std::atomic<bool> presentedDirectly { false };
    // Frames presented directly so far: a frame app_server draws while one
    // goes into the screen may land on top of it.
    std::atomic<uint32_t> directFrames { 0 };
    // A viewDirectExposedHaiku is on its way to the view.
    std::atomic<bool> directExposurePending { false };
    // This reference is read, assigned and released only on the WebKit main
    // thread. Native windows retain the state without touching the client.
    RefPtr<WebView> client;
    // Read, invoked and cleared only on the WebKit main thread.
    RefPtr<SharedTask<void(const WebCore::IntSize&)>> contentSizeChanged;
    // Main-thread load bookkeeping, separate from the close-handshake
    // generation: policy/start/commit can all describe the same navigation.
    struct LoadState {
        uint64_t navigationIdentifier { 0 };
        uint64_t generation { 0 };
        bool identified { false };
        bool pending { false };
        bool stopped { false };
        bool committed { false };
        std::optional<WebCore::ScriptExecutionContextIdentifier> sameDocumentIdentifier;
        String sameDocumentURL;
        const char* outcome { "idle" };
        String errorDescription;
        String errorDomain;
        String errorURL;
        int errorCode { 0 };
        bool errorProvisional { false };
        // The server certificate that failed verification, when that is why
        // the load failed: what the user needs to decide whether to trust it.
        struct Certificate {
            String host;
            String sha256; // hex, of the DER leaf certificate
            String subject;
            String issuer;
            String problem;
            String names; // DNS names and addresses it is valid for, comma-separated
            double validFrom { 0 }; // seconds since the epoch
            double validUntil { 0 };
        };
        std::optional<Certificate> errorCertificate;
        // Keep the last successful load in every snapshot so a dropped
        // progress notification cannot turn a failed load into a visit.
        uint64_t successSequence { 0 };
        String successURL;
        String successTitle;
    } load;
    // Certificates this view's requests rejected lately (main thread): the
    // challenge carries the certificate, the failed load only an error code.
    std::vector<LoadState::Certificate> rejectedCertificates;
    // These close-handshake flags are also confined to the WebKit main thread.
    bool closeRequestPending { false };
    bool closeRequestApproved { false };
    bool closeDecisionInFlight { false };
    uint64_t closeRequestGeneration { 0 };
    uint64_t navigationGeneration { 0 };
    uint64_t closeNavigationGeneration { 0 };
    // The policy callback for a native request is asynchronous. Its matching
    // identifier describes the request we already observed, not a new one.
    std::optional<WebCore::NavigationIdentifier> nativeNavigationIdentifier;
    std::optional<WebCore::ScriptExecutionContextIdentifier> documentIdentifier;
    std::optional<WebCore::ScriptExecutionContextIdentifier> closeDocumentIdentifier;
    bool closeApprovalInvalidated { false };
};

// The density app_server draws the screen at: 2 on 4K monitors at 200%.
// Pages render at this device scale so that their frames reach the screen
// pixel for pixel instead of being enlarged. Cached; safe on any thread.
float displayScaleFactorHaiku();

void commitViewCloseHaiku(std::vector<std::shared_ptr<WebViewStateHaiku>>, BMessenger, uint64_t);

void dispatchViewMessageHaiku(std::shared_ptr<WebViewStateHaiku>, BMessage&&);

// Main thread only. Answer a B_WEBKIT_NEW_PAGE_REQUESTED identifier with this
// view's state, or decline it.
void openNewPageHaiku(std::shared_ptr<WebViewStateHaiku>, uint64_t identifier);
void declineNewPageHaiku(uint64_t identifier);
// Main thread only. Runs an extension item of the last B_WEBKIT_CONTEXT_MENU.
void performExtensionMenuItemHaiku(uint64_t token);

} // namespace WebKit
