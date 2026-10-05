/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WKBase.h"
#include "WebKitExtensionPermission.h"
#include <Messenger.h>
#include <SupportDefs.h>
#include <memory>
#include <string>
#include <vector>

class BWebKitView;
class BRect;

namespace WebKit { class WebViewContextHaiku; }

// Download notifications contain identifier (uint64), url/path/filename (strings),
// current_size (uint64), expected_size (int64; -1 means unknown), and an optional
// originating view messenger. FINISHED also contains result (uint32), error
// (int32), error_domain/error_description (strings). There is one terminal event
// per registered download. The listener must outlive its pending downloads.
// STARTED registers the operation; path may remain empty until PROGRESS reports
// that the NetworkProcess has created the destination. FINISHED always carries
// the final counters, including transfers too short for a progress update.
enum : uint32 {
    B_WEBKIT_DOWNLOAD_STARTED = 'wdst',
    B_WEBKIT_DOWNLOAD_PROGRESS = 'wdpr',
    B_WEBKIT_DOWNLOAD_FINISHED = 'wdfn',
    B_WEBKIT_BROWSER_WINDOW_UPDATED = 'wbwu',
    B_WEBKIT_EXTENSION_PACKAGE_PREPARED = 'wepk',
    B_WEBKIT_EXTENSION_PACKAGE_DISCARDED = 'wepd',
    B_WEBKIT_EXTENSION_LOADED = 'weld',
    B_WEBKIT_EXTENSION_UNLOADED = 'weul',
    B_WEBKIT_EXTENSION_ACTIONS_CHANGED = 'weac',
    B_WEBKIT_EXTENSION_ACTIONS = 'weas',
    B_WEBKIT_EXTENSION_ACTION_ACTIVATED = 'weaa'
};
enum : uint32 {
    B_WEBKIT_DOWNLOAD_SUCCEEDED,
    B_WEBKIT_DOWNLOAD_FAILED,
    B_WEBKIT_DOWNLOAD_CANCELLED
};

// Browser commands ask the host to change its own tab and window model for a
// loaded extension (tabs.create/update/remove/reload, windows.create/update/
// remove). The engine never opens, closes, selects or navigates a host tab by
// itself. COMMAND contains identifier (uint64), command (uint32, below),
// deadline (int64, system_time microseconds) and extension_identifier/
// extension_name strings. Tabs are named by the "view" messenger of their
// BWebKitView plus a parallel page_identifier (uint64); windows by the "window"
// messenger given to SetBrowserWindowTabs plus window_identifier (uint64).
//   OPEN_TAB       optional url (absent: the host's new-tab page), active (bool),
//                  index (int32, -1 appends), optional window/window_identifier
//                  (absent: host's choice), optional opener view/page.
//   CLOSE_TABS     repeated view/page_identifier and parallel window entries.
//                  Succeeds only when every named tab was closed; a user who
//                  keeps a tab open through beforeunload answers B_CANCELED.
//   ACTIVATE_TAB   view, window. Select the tab; do not raise the window.
//   NAVIGATE_TAB   view, url.       RELOAD_TAB  view, bypass_cache (bool).
//   GO_BACK / GO_FORWARD  view.
//   OPEN_WINDOW    repeated url (may be absent), repeated view for existing
//                  tabs to adopt (hosts may answer B_NOT_SUPPORTED), focused
//                  (bool), type ("normal"/"popup"), state, private (bool).
//   CLOSE_WINDOW / FOCUS_WINDOW  window, window_identifier.
//   SET_WINDOW_STATE  window, state ("normal", "minimized", "maximized",
//                  "fullscreen").
//   MOVE_TAB       view, window, index (int32; -1: after the last tab). Moves
//                  the tab within its own window.
// Answer every command exactly once with RespondToBrowserCommand, after the
// change is visible through SetBrowserWindowTabs. CANCELLED (identifier only)
// tells the host the engine stopped waiting: the deadline passed, the extension
// unloaded or the listener changed. A late answer is ignored. Unknown commands
// must be answered with B_NOT_SUPPORTED.
enum : uint32 {
    B_WEBKIT_BROWSER_COMMAND = 'wbcm',
    B_WEBKIT_BROWSER_COMMAND_CANCELLED = 'wbcc'
};
enum : uint32 {
    B_WEBKIT_BROWSER_OPEN_TAB = 1,
    B_WEBKIT_BROWSER_CLOSE_TABS,
    B_WEBKIT_BROWSER_ACTIVATE_TAB,
    B_WEBKIT_BROWSER_NAVIGATE_TAB,
    B_WEBKIT_BROWSER_RELOAD_TAB,
    B_WEBKIT_BROWSER_GO_BACK,
    B_WEBKIT_BROWSER_GO_FORWARD,
    B_WEBKIT_BROWSER_OPEN_WINDOW,
    B_WEBKIT_BROWSER_CLOSE_WINDOW,
    B_WEBKIT_BROWSER_FOCUS_WINDOW,
    B_WEBKIT_BROWSER_SET_WINDOW_STATE,
    B_WEBKIT_BROWSER_MOVE_TAB
};

// Site permissions: what pages may ask the user for, and the services that
// depend on the answer. All go to the listener set with SetPermissionListener.
//   PERMISSION_REQUESTED  identifier (uint64), permission ("notifications",
//                  "geolocation", "camera", "microphone", "camera-microphone"
//                  or "screen"), origin ("scheme://host[:port]"), private
//                  (bool), view (the asking BWebKitView) when known. Answer
//                  once with RespondToPermissionRequest; origins with a saved
//                  decision (SetSitePermission) are answered without asking.
//                  "camera-microphone" asks for both at once; its answer is
//                  saved as "camera" and "microphone". "screen" (getDisplayMedia)
//                  is never saved: it carries repeated "device_id" and
//                  "device_label" strings, the screens that may be shared, and
//                  "audio" (bool, the page also asked for the sound); answer it
//                  with RespondToMediaPermissionRequest and the chosen id.
//   NOTIFICATION_SHOW  identifier (uint64), title, body, tag, icon_url, lang,
//                  origin strings, rtl and silent bools, view when its page is
//                  known. Show it (BNotification); report a click with
//                  NotificationClicked and a dismissal with NotificationClosed.
//                  A notification with the tag of a shown one replaces it.
//   NOTIFICATION_CLOSE  identifier: the page closed it.
//   GEOLOCATION_START  high_accuracy (bool): pages are watching the position.
//                  Answer with SetGeolocationPosition (again whenever it
//                  changes) or GeolocationUnavailable. Sent again when the
//                  accuracy wanted changes.
//   GEOLOCATION_STOP   no page is watching any more.
enum : uint32 {
    B_WEBKIT_PERMISSION_REQUESTED = 'wprq',
    B_WEBKIT_NOTIFICATION_SHOW = 'wnsh',
    B_WEBKIT_NOTIFICATION_CLOSE = 'wncl',
    B_WEBKIT_GEOLOCATION_START = 'wgst',
    B_WEBKIT_GEOLOCATION_STOP = 'wgsp'
};

struct BWebKitExtensionLoadOptions {
    enum class Purpose : uint32 { UserApproved, BrowserStartup, BrowserUpdate };
    std::string uniqueIdentifier;
    std::string expectedFingerprint;
    Purpose purpose { Purpose::UserApproved };
    std::vector<std::string> permissions;
    std::vector<std::string> origins;
    bool allowFileURLs { false };
    bool allowPrivateBrowsing { false };
};

// Stable host installation identity and precedence, independent of load order.
// Both values must be supplied together. The identifier changes on reinstall;
// order increases for later installations and is retained across disable/restart.
// The overload without metadata keeps its existing ABI and behavior.
struct BWebKitExtensionInstallation {
    std::string identifier;
    uint64 order { 0 };
};

// Create on the BApplication thread after BWebKitInitialize(). All views given
// this context share one website data store and process pool. A persistent
// profile path must be absolute; nullptr selects the application's default
// WebKit profile. A private context ignores the path and creates no directories.
class WK_EXPORT BWebKitContext final {
public:
    explicit BWebKitContext(const char* profilePath, bool privateBrowsing = false);
    // A private context whose pages the extensions of extensionHost serve:
    // one runtime of each extension (its background page, storage and
    // state) spans normal and private windows, as in other browsers. Only
    // extensions allowed in private browsing (allowPrivateBrowsing when
    // loaded, or SetExtensionPrivateAccess) see its pages, get them as tabs,
    // inject content into them or show actions for them. Extensions are
    // loaded into extensionHost, never into this context; submit its
    // windows with SetBrowserWindowTabs as usual. extensionHost must not be
    // private; this context keeps it alive.
    explicit BWebKitContext(const std::shared_ptr<BWebKitContext>& extensionHost);
    ~BWebKitContext();
    BWebKitContext(const BWebKitContext&) = delete;
    BWebKitContext& operator=(const BWebKitContext&) = delete;

    status_t InitCheck() const;
    bool IsPrivate() const;

    // Submit from the owning window looper while these views are alive. The
    // complete ordered list is applied on WebKit main after checking every
    // page belongs to this context. An empty list removes the window. A null
    // activeView records no active tab. Closed/missing/mixed-profile views
    // reject the whole update. Optional reply: identifier, error, and on
    // success window_identifier, tab_count and active_index (-1 for none).
    void SetBrowserWindowTabs(const BMessenger& window, const std::vector<BWebKitView*>&,
        BWebKitView* activeView, bool focused, const BMessenger& reply = BMessenger(), uint64 identifier = 0);

    // Configure on the application thread. A local listener receives
    // B_WEBKIT_BROWSER_COMMAND requests; an invalid messenger clears it and
    // extensions then get "not implemented" errors instead of waiting. Changing
    // the listener fails its outstanding commands. B_NOT_SUPPORTED means this
    // WebKit build has extensions disabled.
    status_t SetBrowserCommandListener(const BMessenger&);
    // Answer from any looper; B_OK as status means the command was performed.
    // createdViews names the tabs made for OPEN_TAB (exactly one) or
    // OPEN_WINDOW (one or more); call from the looper owning those live views,
    // after submitting SetBrowserWindowTabs for their window, so the engine can
    // resolve them. error optionally explains a failure to the extension.
    // Unknown, answered and expired identifiers are ignored.
    void RespondToBrowserCommand(uint64 identifier, status_t status,
        const std::vector<BWebKitView*>& createdViews = { }, const char* error = nullptr);

    // Configure permission consent on the application thread. B_NOT_SUPPORTED
    // means this WebKit build has extensions disabled. B_BUSY prevents listener
    // replacement while requests are pending. Only local targets are accepted.
    status_t SetExtensionPermissionListener(const BMessenger&);
    // Replies/cancellation can be submitted from any looper. Only a live,
    // displayed request accepts a decision; duplicate/stale IDs are ignored.
    void RespondToExtensionPermissionRequest(const char* identifier, bool allowed);
    void CancelExtensionPermissionRequests();

    // Submit from any looper; B_OK queues work, it does not indicate success.
    // reply must be a live local messenger; identifier must be in [1, UINT64_MAX-1].
    // PREPARED contains identifier/error and, on success, token/name/version/
    // description/fingerprint strings, manifest_version (double), the complete
    // parsed/localized manifest_json, verified_crx_id (empty for other formats),
    // and repeated recognized permission/origin
    // strings (including content-script origins). Inspect manifest_json for
    // requirements WebKit does not recognize. The token owns a package snapshot
    // in this context. CRX3 loads must use the verified signer ID. This verifies
    // package integrity/identity, not Chrome Web Store provenance.
    // Preparation grants no permissions and executes no code.
    // At most eight preparations/snapshots may be outstanding per context.
    status_t PrepareExtension(const char* path, const BMessenger& reply, uint64 identifier);
    void CancelExtensionPreparation(uint64 identifier);
    // Release a prepared token. Optional DISCARDED reply contains identifier/error.
    void DiscardPreparedExtension(const char* token, const BMessenger& reply = BMessenger(), uint64 identifier = 0);
    // Query on the application thread after queued looper submissions drain.
    // Cancellation replies immediately but
    // disk work may still be finishing. Keep the context and RunLoop alive
    // until this returns false before application shutdown.
    bool HasPendingExtensionPreparations() const;

    // Activate exactly the approved prepared snapshot. The host supplies a
    // stable identity and the fingerprint from its consent/installation record.
    // UserApproved replaces saved grants with these explicit permission/origin
    // lists, including empty lists. Startup/update require empty lists and
    // restore saved grants only when the recorded resource fingerprint matches.
    // Private contexts require allowPrivateBrowsing and explicit UserApproved
    // grants. File URL access is independent and defaults to disabled.
    // LOADED carries identifier/error, extension_identifier, and on success
    // fingerprint/base_url. Success consumes the preparation token and means
    // the controller loaded the context; background execution is asynchronous.
    status_t LoadPreparedExtension(const char* token, const BWebKitExtensionLoadOptions&,
        const BMessenger& reply, uint64 identifier);
    // The host obtains these values from its saved installation catalog.
    // LOADED also reports installation_identifier and installation_order, and
    // new_tab_url when the manifest overrides the new tab page.
    status_t LoadPreparedExtension(const char* token, const BWebKitExtensionLoadOptions&,
        const BWebKitExtensionInstallation&, const BMessenger& reply, uint64 identifier);
    // Create on the application thread after an extension has loaded. This
    // dedicated view uses that extension's CSP, origin, and privileged context;
    // its top-level navigation stays within the extension. The caller owns the
    // returned view and may attach it to a native window. Returns nullptr and
    // an optional status on failure; notifications use the usual view protocol.
    BWebKitView* CreateExtensionView(BRect, const char* name, const char* extensionIdentifier,
        const BMessenger& notificationTarget, status_t* error = nullptr);

    // UNLOADED carries identifier/error/extension_identifier. This disables the
    // runtime; it does not remove the installed package or its stored data.
    status_t UnloadExtension(const char* extensionIdentifier, const BMessenger& reply, uint64 identifier);
    // Lets a loaded extension see and act on private pages (those of the
    // contexts created from this one), or stops it, at once: its content
    // scripts and rules are added to or taken from open private pages and
    // CHANGED is sent to the action listener. The host keeps the choice and
    // passes it as allowPrivateBrowsing at the next load. Any looper; an
    // extension that is not loaded is ignored.
    status_t SetExtensionPrivateAccess(const char* extensionIdentifier, bool allowed);

    // Configure on the application thread. A local listener receives CHANGED
    // invalidations after load/unload or live action-property changes. An
    // invalid messenger clears it. Query again after changing the active tab.
    status_t SetExtensionActionListener(const BMessenger&);
    // Submit from any looper. ACTIONS contains identifier/error and repeated
    // action messages for this window's active page. Each action contains
    // extension_identifier/name/title/badge strings, enabled/has_popup bools,
    // load_identifier and page_identifier uint64 values, and badge_background_rgba
    // and badge_text_rgba uint32 colors packed as 0xRRGGBBAA. Optional icon_bgra is
    // exactly 16*16*4 bytes of unpremultiplied BGRA pixels, top row first, and
    // optional icon_bgra_32 the same at 32*32 for a high density screen.
    status_t GetExtensionActions(const BMessenger& window, const BMessenger& reply, uint64 identifier);
    // Trusted native user input only. Both IDs must come from the displayed
    // snapshot. The window must still be focused on that page, the extension
    // must still have that load identity and its current action must be enabled.
    // B_OK in ACTIVATED means dispatch accepted; popup loading is asynchronous.
    // This grants the normal action user gesture/activeTab permission.
    status_t ActivateExtensionAction(const char* extensionIdentifier, const BMessenger& window,
        uint64 loadIdentifier, uint64 pageIdentifier, const BMessenger& reply, uint64 identifier);
    // Load the action's popup page hidden before the button is clicked (call
    // when the pointer enters the button); the click then only shows it. Same
    // identity checks as ActivateExtensionAction, but no user gesture, no
    // activeTab grant and no reply. Ignored while that extension has a popup.
    status_t PreloadExtensionAction(const char* extensionIdentifier, const BMessenger& window,
        uint64 loadIdentifier, uint64 pageIdentifier);

    // Configure on the application thread before downloads start. The directory
    // must already exist, be writable, and have an absolute UTF-8 path. Existing
    // files are never overwritten. Both setters return B_BUSY while downloads
    // are pending. No directory is chosen implicitly, including in private mode.
    status_t SetDownloadListener(const BMessenger&);
    status_t SetDownloadDirectory(const char*);
    // Cancellation may be requested from any looper while this context is alive.
    // Completion is asynchronous. An engine finish/failure that wins the race
    // retains its result; otherwise the terminal result is CANCELLED.
    void CancelDownload(uint64 identifier);
    void CancelAllDownloads();
    // Query on the application thread after queued view commands have drained.
    // Keep its RunLoop and this context alive until cancellation replies finish.
    bool HasPendingDownloads() const;

    // Removes B_WEBKIT_DATA_* (WebKitEmbedding.h) of every page of this
    // context, of all time, and answers B_WEBKIT_WEBSITE_DATA_REMOVED
    // ("identifier", "error" on failure) to reply. Storage that extensions keep
    // under their own webkit-extension: origins (local storage, IndexedDB,
    // service workers) survives unless includeExtensions is true, as clearing a
    // browser's site data does not reset its extensions. Any looper.
    void RemoveWebsiteData(uint32 types, const BMessenger& reply, uint64 identifier, bool includeExtensions = false);

    // Trusts a server certificate that failed verification (self-signed,
    // expired, issued for another name or by an unknown authority) for host,
    // as the user decided after a load failed with "loadErrorCertificate"
    // (WebKitView.h). sha256 is that notification's
    // "loadErrorCertificateSHA256". Requests of this context's pages to host
    // then accept that certificate, and only that one; a changed certificate
    // fails again. Not stored: the host keeps its users' decisions and gives
    // them again after a restart. Any thread.
    void AllowServerCertificate(const char* host, const char* sha256);
    // Withdraws AllowServerCertificate() for host, or for every host when host
    // is null or empty. Any thread.
    void ForgetServerCertificates(const char* host = nullptr);

    // Site permissions (B_WEBKIT_PERMISSION_REQUESTED and the messages after
    // it). Any thread; an invalid messenger denies every request. Requests
    // waiting for an answer are denied when the listener changes.
    status_t SetPermissionListener(const BMessenger&);
    void RespondToPermissionRequest(uint64 identifier, bool allowed);
    // Answers a "screen" request with the device_id the user chose (or any
    // request; the device is ignored for the others).
    void RespondToMediaPermissionRequest(uint64 identifier, bool allowed, const char* deviceId);
    // The host's decision for an origin: state 1 allows, 0 denies, -1 forgets
    // it (the page asks again). Pages see it at once (Notification.permission,
    // navigator.permissions.query()). The engine stores nothing: give the
    // saved decisions again after every start. Any thread.
    void SetSitePermission(const char* permission, const char* origin, int32 state);
    void NotificationClicked(uint64 identifier);
    void NotificationClosed(uint64 identifier);
    // A position in degrees, its accuracy in metres and when it was taken
    // (seconds since the epoch). Any thread.
    void SetGeolocationPosition(double latitude, double longitude, double accuracy, double timestamp);
    void GeolocationUnavailable(const char* message);

private:
    friend class BWebKitView;
    std::shared_ptr<WebKit::WebViewContextHaiku> m_context;
    status_t m_status { B_NO_INIT };
    const bool m_private;
};
