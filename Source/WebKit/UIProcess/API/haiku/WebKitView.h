/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WKBase.h"
#include "WebKitContext.h"
#include "WebKitEmbedding.h"
#include <DirectWindow.h>
#include <Messenger.h>
#include <View.h>
#include <memory>
#include <vector>

namespace WebKit { struct WebViewStateHaiku; class JavaScriptDialogHostHaiku; class ExtensionActionPopupHaiku; }
class BCursor;
class BWebKitInspectorSession;

// Call on the BApplication thread before creating any modern web views.
WK_EXPORT status_t BWebKitInitialize();
// Query on the application thread while draining close operations. Keep the
// application loop alive until native windows and popup workers finish. After
// observing false, queue one final application turn to drain completions that
// native UI has already submitted.
WK_EXPORT bool BWebKitHasPendingNativeUI();
// Frame buffer pixels per logical pixel of the screen (2 on 4K monitors at
// 200%). Bitmaps this many times their logical size, drawn into a logical
// rectangle, reach the screen pixel for pixel. Any thread; cached for a second.
WK_EXPORT float BWebKitDisplayScale();

enum : uint32 {
    B_WEBKIT_STATE_CHANGED = 'wsta',
    B_WEBKIT_PROCESS_EXITED = 'wpex',
    B_WEBKIT_FIND_RESULT = 'wfin',
    B_WEBKIT_CLOSE_REQUESTED = 'wclr',
    B_WEBKIT_CLOSE_CANCELLED = 'wclc',
    B_WEBKIT_CLOSE_PROMPT = 'wclp',
    B_WEBKIT_CLOSE_COMMITTED = 'wclb',
    // An extension page navigated its view to a web page, which only an ordinary
    // view can display. Fields: "view" (messenger), "url" (string). The navigation
    // was not started; the host is expected to show the URL in a web view instead.
    B_WEBKIT_EXTERNAL_NAVIGATION_REQUESTED = 'wexn',
    // The main frame's favicon was loaded. Fields: "view" (messenger), "url"
    // (the page the icon belongs to), "iconURL", "mimeType" (may be empty) and
    // "data" (B_RAW_TYPE, the undecoded icon file, at most 1 MiB).
    B_WEBKIT_ICON_LOADED = 'wico',
    // The user asked for a context menu on the page (secondary button) and the
    // page did not cancel it. The host shows its own menu. Fields: "view",
    // "where" (BPoint, view coordinates), "main_frame" (bool), and when present
    // "frame_url", "selected_text", "link_url", "link_label", "link_title",
    // "link_filename", "image_url", "image_filename", "media_url" (strings),
    // "editable", "selected", "downloadable_media" (bool). Extensions' own
    // items (menus API) that apply come as repeated "extension" messages:
    // "name" and repeated "item" messages with "title", "enabled",
    // "checkable", "checked", and either "token" (uint64, for
    // PerformContextMenuExtensionItem), nested "item"s (a submenu) or
    // "separator".
    B_WEBKIT_CONTEXT_MENU = 'wctx',
    // A page asked for a new page: window.open(), or a link or form with a
    // target. Fields: "view" (the opener), "identifier" (uint64), "url" (may be
    // about:blank), "popup" (bool: the page asked for a sized pop-up window),
    // "user_gesture" (bool), "modifiers" and "button" (native bits of the click,
    // if any), optional "width"/"height" and "x"/"y" (floats, screen units).
    // Answer exactly once: create a view with the identifier (the engine then
    // loads the page itself; do not call LoadURL) or call DeclineNewPage().
    // The opener's script waits until then.
    B_WEBKIT_NEW_PAGE_REQUESTED = 'wnpg',
    // A middle click or Command-click on a link. The page did not navigate.
    // Fields: "view", "url", "modifiers", "button" (native bits).
    B_WEBKIT_LINK_OPEN_REQUESTED = 'wlop',
    // The pointer moved on to or off a link. Fields: "view", "url" (empty when
    // it left the link), optional "title".
    B_WEBKIT_LINK_HOVERED = 'wlhv'
};

// Navigation and input are queued to the application thread. Notifications are
// sent to the supplied messenger with a "view" messenger and page state fields.
class WK_EXPORT BWebKitView : public BView {
public:
    BWebKitView(BRect, const char* name, BMessenger notificationTarget,
        uint32 resizingMode = B_FOLLOW_ALL, std::shared_ptr<BWebKitContext> = nullptr);
    // Shows the page of a B_WEBKIT_NEW_PAGE_REQUESTED notification. If the
    // request is no longer pending, the view starts empty.
    BWebKitView(BRect, const char* name, BMessenger notificationTarget, uint64 newPageRequest,
        uint32 resizingMode = B_FOLLOW_ALL, std::shared_ptr<BWebKitContext> = nullptr);
    // Refuses a B_WEBKIT_NEW_PAGE_REQUESTED notification; window.open() then
    // returns null. Callable from any thread.
    static void DeclineNewPage(uint64 newPageRequest);
    // Runs an extension item chosen from the last B_WEBKIT_CONTEXT_MENU, as
    // a user action. Tokens of older menus are ignored. Any thread.
    static void PerformContextMenuExtensionItem(uint64 token);
    ~BWebKitView() override;
    status_t InitCheck() const;
    void LoadURL(const char*);
    // Queue an explicit download of a valid URL without changing this page.
    // Uses this view's context, configured download directory and listener.
    void DownloadURL(const char*);
    // Opens a connection to the server of an https URL the user is likely to
    // load soon (DNS, TCP and TLS), so that load's first request goes out at
    // once. Nothing is sent to the server. Any thread.
    void Preconnect(const char*);
    void GoBack();
    void GoForward();
    void Reload();
    void Stop();
    // Asks beforeunload handlers before notifying CLOSE_REQUESTED or
    // CLOSE_CANCELLED. Keep the view alive until the resulting notification.
    // Repeated requests while waiting for a decision are ignored.
    void RequestClose();
    // Withdraw a completed provisional approval when a window quit is cancelled.
    // Does not reset a close decision that is still pending in the WebProcess.
    void ResetCloseRequest();
    // Retain all native views until CLOSE_COMMITTED returns identifier and
    // closed. On main, validate all approvals against observed navigation and
    // close every engine page together; a failed validation closes none.
    static void CommitClose(const std::vector<BWebKitView*>&, BMessenger target, uint64 identifier);
    // The result contains view, query, identifier, error, and found on success.
    // A newer request, explicit navigation or HideFindUI cancels older replies.
    void FindString(const char*, bool forward = true, bool caseSensitive = false, bool wrapSelection = true);
    void HideFindUI();
    void IncreaseZoomFactor(bool textOnly = false);
    void DecreaseZoomFactor(bool textOnly = false);
    void ResetZoomFactor();
    // Accepts B_CUT, B_COPY, B_PASTE, B_SELECT_ALL, B_UNDO and B_REDO.
    void ExecuteEditCommand(uint32);

    // Embedding: see WebKitEmbedding.h. Scripts run in the pages' own world.
    void AddUserScript(const char* source, bool atDocumentStart = true, bool allFrames = true);
    void RemoveAllUserScripts();
    // Offers window.webkit.messageHandlers.<name> to the view's pages; their
    // messages reach target as B_WEBKIT_SCRIPT_MESSAGE.
    void AddScriptMessageHandler(const char* name, BMessenger target);
    void RemoveScriptMessageHandler(const char* name);
    // Resolves the page's postMessage() promise with reply, a string the page
    // receives unchanged, or rejects it with error when that is not null.
    // Any thread; unknown and answered tokens are ignored.
    static void ReplyToScriptMessage(uint64 replyToken, const char* reply, const char* error = nullptr);
    // Runs source in the main frame and sends B_WEBKIT_JAVASCRIPT_RESULT to
    // reply. As an async function, source is a function body whose returned
    // promise is awaited.
    void EvaluateJavaScript(const char* source, BMessenger reply, uint64 identifier, bool asAsyncFunction = false);
    // An empty string restores the engine's own user agent.
    void SetCustomUserAgent(const char*);
    // The view's pages load URLs of scheme through target, once per scheme and
    // view, and never for schemes the engine loads itself (http, file...).
    void SetURLSchemeHandler(const char* scheme, BMessenger target, uint32 flags = 0);
    // Answers a B_WEBKIT_URL_SCHEME_REQUEST whole. headers holds repeated
    // "name"/"value" strings; a null mimeType is guessed from the URL.
    // Any thread; stale identifiers are ignored.
    static void RespondToURLSchemeRequest(uint64 identifier, int32 status, const char* mimeType,
        const BMessage* headers, const void* data, size_t size);
    static void FailURLSchemeRequest(uint64 identifier, const char* description = nullptr);
    // How a beforeunload handler that asks to stay is answered; notifications
    // (B_WEBKIT_BEFORE_UNLOAD_PREVENTED) go to target.
    void SetBeforeUnloadBehavior(uint32 behavior, BMessenger target = BMessenger());
    // Page zoom as a factor (1 is 100%), as Electron's setZoomFactor.
    void SetZoomFactor(double factor);
    // The cookies of this view's website data (its BWebKitContext), including
    // HttpOnly ones: for the URL, or all when url is null or empty. Answered
    // with B_WEBKIT_COOKIES to reply.
    void GetCookies(const char* url, BMessenger reply, uint64 identifier);
    // A cookie as the fields of B_WEBKIT_COOKIES; B_WEBKIT_COOKIES_CHANGED.
    void SetCookie(const BMessage& cookie, BMessenger reply, uint64 identifier);
    // Cookies of the URL (every cookie when url is empty), only those named
    // name when it is not empty; B_WEBKIT_COOKIES_CHANGED.
    void DeleteCookies(const char* url, const char* name, BMessenger reply, uint64 identifier);
    // B_WEBKIT_DATA_* of the view's website data, of all time;
    // B_WEBKIT_WEBSITE_DATA_REMOVED.
    void RemoveWebsiteData(uint32 types, BMessenger reply, uint64 identifier);
    // Call from BDirectWindow::DirectConnected() of a window made with
    // B_DIRECT_DEVICE_PIXELS. Its web views then have their pages copied
    // straight into the screen by the GPU wherever they are visible, instead
    // of read back from the GPU and drawn by app_server. Any thread; returns
    // once nothing more lands where the window no longer is. A null info, or
    // a window that is not told the screen's density, presents as before.
    static void WindowDirectConnected(BWindow* window, const direct_buffer_info* info);

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void FrameResized(float, float) override;
    void FrameMoved(BPoint) override;
    void WindowActivated(bool) override;
    void MakeFocus(bool = true) override;
    void Show() override;
    void Hide() override;
    void Draw(BRect) override;
    void KeyDown(const char*, int32) override;
    void KeyUp(const char*, int32) override;
    void MouseDown(BPoint) override;
    void MouseUp(BPoint) override;
    void MouseMoved(BPoint, uint32, const BMessage*) override;
    void MessageReceived(BMessage*) override;

private:
    friend class BWebKitContext;
    friend class BWebKitInspectorSession;
    friend class WebKit::ExtensionActionPopupHaiku;
    BWebKitView(BRect, const char*, std::shared_ptr<WebKit::WebViewStateHaiku>);
    void UpdateGeometry();
    void SendInput(const BMessage&, BPoint = BPoint());
    // Offers files dragged from another application to the page.
    void SendDrag(const BMessage& drag, BPoint, const char* phase);
    void SendCommand(uint32, const char* = nullptr);
    void ChangeZoomFactor(int32 direction, bool textOnly);
    std::shared_ptr<WebKit::WebViewStateHaiku> m_state;
    std::unique_ptr<BCursor> m_cursor;
    std::unique_ptr<WebKit::JavaScriptDialogHostHaiku> m_dialogHost;
    status_t m_status { B_NO_INIT };
    bool m_failedViewCloseNotified { false };
};
