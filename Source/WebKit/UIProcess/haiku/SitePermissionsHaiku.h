/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <Messenger.h>
#include <SupportDefs.h>
#include "WebNotificationIdentifier.h"
#include <WebCore/PermissionState.h>
#include <memory>
#include <optional>
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

class WebGeolocationManagerProxy;
class WebNotification;
class WebPageProxy;
class WebProcessPool;

// What a page may ask the user for (notifications and location), the host's
// answers, and the two services that depend on them: web notifications, which
// the host shows (BNotification), and positions, which the host finds. One per
// WebViewContextHaiku, installed on its process pool. Main thread only; the
// BWebKitContext entry points dispatch there.
class SitePermissionsHaiku final : public std::enable_shared_from_this<SitePermissionsHaiku> {
public:
    // Camera and microphone decisions are saved like the others; sharing a
    // screen is asked every time (requestMedia()).
    enum class Kind : uint8_t { Notifications, Geolocation, Camera, Microphone };
    static constexpr size_t kindCount = 4;
    static std::shared_ptr<SitePermissionsHaiku> create(WebProcessPool&, bool privateBrowsing);
    ~SitePermissionsHaiku();

    static std::optional<Kind> kindForName(const String&);
    static ASCIILiteral nameForKind(Kind);

    void setListener(const BMessenger&);
    // The host's saved decision for origin ("scheme://host[:port]"): allowed,
    // denied, or none (the page asks again).
    void setDecision(Kind, const String& origin, std::optional<bool> allowed);
    std::optional<bool> decision(Kind, const String& origin) const;
    HashMap<String, bool> decisions(Kind) const;

    // A page asks: answered from a saved decision, or by the host, or denied
    // when there is no host. view is the asking page's BWebKitView.
    void request(Kind, const String& origin, const BMessenger& view, CompletionHandler<void(bool)>&&);
    void respond(uint64_t identifier, bool allowed, const String& deviceId = { });

    // getUserMedia() and getDisplayMedia(): the camera and microphone from
    // saved decisions or the host; a screen always from the host, which
    // answers with the one chosen among `screens` (id, label).
    struct MediaRequest {
        bool camera { false };
        bool microphone { false };
        bool screen { false };
        bool screenAudio { false };
        Vector<std::pair<String, String>> screens;
    };
    using MediaCompletion = CompletionHandler<void(bool allowed, const String& deviceId)>;
    void requestMedia(const String& origin, const BMessenger& view, MediaRequest&&, MediaCompletion&&);
    // navigator.permissions.query(): granted, denied or prompt.
    std::optional<WebCore::PermissionState> query(Kind, const String& origin) const;

    // Notifications (NotificationProviderHaiku).
    bool showNotification(WebPageProxy*, WebNotification&);
    void cancelNotification(WebNotification&);
    void forgetNotification(WebNotification&);
    void notificationClicked(uint64_t identifier);
    void notificationClosed(uint64_t identifier);

    // Geolocation (GeolocationProviderHaiku).
    void geolocationStart(WebGeolocationManagerProxy&);
    void geolocationStop();
    void geolocationAccuracy(bool high);
    void setPosition(double latitude, double longitude, double accuracy, double timestamp);
    void positionUnavailable(const String& message);

    void shutdown();

private:
    SitePermissionsHaiku(WebProcessPool&, bool privateBrowsing);
    void install();
    bool post(BMessage&) const;
    void updateWebProcesses(Kind, const String& origin, std::optional<bool> allowed);

    WebProcessPool& m_processPool;
    const bool m_private;
    BMessenger m_listener;
    HashMap<String, bool> m_decisions[kindCount];
    struct Pending {
        Kind kind;
        String origin;
        CompletionHandler<void(bool)> completion;
        MediaCompletion mediaCompletion;
    };
    HashMap<uint64_t, Pending> m_pending;
    uint64_t m_lastRequest { 0 };
    // Shown notifications by the identifier given to the host.
    HashMap<uint64_t, WebNotificationIdentifier> m_notifications;
    WebGeolocationManagerProxy* m_geolocation { nullptr };
    bool m_highAccuracy { false };
    bool m_shutdown { false };
};

} // namespace WebKit
