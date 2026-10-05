/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "SitePermissionsHaiku.h"

#include "APIArray.h"
#include "APIGeolocationProvider.h"
#include "APINotificationProvider.h"
#include "APINumber.h"
#include "APISecurityOrigin.h"
#include "WebGeolocationManagerProxy.h"
#include "WebGeolocationPosition.h"
#include "WebKitContext.h"
#include "WebNotification.h"
#include "WebNotificationManagerProxy.h"
#include "WebPageProxy.h"
#include "WebProcessPool.h"
#include "WebViewPrivate.h"
#include <Message.h>
#include <WebCore/GeolocationPositionData.h>
#include <WebCore/NotificationDirection.h>
#include <WebCore/SecurityOrigin.h>
#include <wtf/RunLoop.h>

namespace WebKit {

namespace {

class NotificationProviderHaiku final : public API::NotificationProvider {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(NotificationProviderHaiku);
public:
    explicit NotificationProviderHaiku(std::weak_ptr<SitePermissionsHaiku> permissions)
        : m_permissions(std::move(permissions)) { }

    bool show(WebPageProxy* page, WebNotification& notification, RefPtr<WebCore::NotificationResources>&&) override
    {
        auto permissions = m_permissions.lock();
        return permissions && permissions->showNotification(page, notification);
    }
    void cancel(WebNotification& notification) override
    {
        if (auto permissions = m_permissions.lock())
            permissions->cancelNotification(notification);
    }
    void didDestroyNotification(WebNotification& notification) override
    {
        if (auto permissions = m_permissions.lock())
            permissions->forgetNotification(notification);
    }
    HashMap<String, bool> notificationPermissions() override
    {
        auto permissions = m_permissions.lock();
        return permissions ? permissions->decisions(SitePermissionsHaiku::Kind::Notifications) : HashMap<String, bool> { };
    }

private:
    const std::weak_ptr<SitePermissionsHaiku> m_permissions;
};

#if ENABLE(GEOLOCATION)
class GeolocationProviderHaiku final : public API::GeolocationProvider {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(GeolocationProviderHaiku);
public:
    explicit GeolocationProviderHaiku(std::weak_ptr<SitePermissionsHaiku> permissions)
        : m_permissions(std::move(permissions)) { }

    void startUpdating(WebGeolocationManagerProxy& manager) override
    {
        if (auto permissions = m_permissions.lock())
            permissions->geolocationStart(manager);
        else
            manager.providerDidFailToDeterminePosition("Location is not available"_s);
    }
    void stopUpdating(WebGeolocationManagerProxy&) override
    {
        if (auto permissions = m_permissions.lock())
            permissions->geolocationStop();
    }
    void setEnableHighAccuracy(WebGeolocationManagerProxy&, bool enabled) override
    {
        if (auto permissions = m_permissions.lock())
            permissions->geolocationAccuracy(enabled);
    }

private:
    const std::weak_ptr<SitePermissionsHaiku> m_permissions;
};
#endif

} // namespace

std::shared_ptr<SitePermissionsHaiku> SitePermissionsHaiku::create(WebProcessPool& processPool, bool privateBrowsing)
{
    std::shared_ptr<SitePermissionsHaiku> permissions(new SitePermissionsHaiku(processPool, privateBrowsing));
    permissions->install();
    return permissions;
}

SitePermissionsHaiku::SitePermissionsHaiku(WebProcessPool& processPool, bool privateBrowsing)
    : m_processPool(processPool)
    , m_private(privateBrowsing)
{
}

SitePermissionsHaiku::~SitePermissionsHaiku() = default;

void SitePermissionsHaiku::install()
{
    ASSERT(RunLoop::isMain());
    if (RefPtr manager = m_processPool.supplement<WebNotificationManagerProxy>())
        manager->setProvider(makeUnique<NotificationProviderHaiku>(weak_from_this()));
#if ENABLE(GEOLOCATION)
    if (RefPtr manager = m_processPool.supplement<WebGeolocationManagerProxy>())
        manager->setProvider(makeUnique<GeolocationProviderHaiku>(weak_from_this()));
#endif
}

void SitePermissionsHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    m_shutdown = true;
    m_listener = BMessenger();
    auto pending = std::exchange(m_pending, { });
    for (auto& request : pending.values()) {
        if (request.completion)
            request.completion(false);
        if (request.mediaCompletion)
            request.mediaCompletion(false, { });
    }
    if (m_geolocation)
        m_geolocation->providerDidFailToDeterminePosition("Location is not available"_s);
    m_geolocation = nullptr;
}

std::optional<SitePermissionsHaiku::Kind> SitePermissionsHaiku::kindForName(const String& name)
{
    if (name == "notifications"_s)
        return Kind::Notifications;
    if (name == "geolocation"_s)
        return Kind::Geolocation;
    if (name == "camera"_s)
        return Kind::Camera;
    if (name == "microphone"_s)
        return Kind::Microphone;
    return std::nullopt;
}

ASCIILiteral SitePermissionsHaiku::nameForKind(Kind kind)
{
    switch (kind) {
    case Kind::Notifications:
        return "notifications"_s;
    case Kind::Geolocation:
        return "geolocation"_s;
    case Kind::Camera:
        return "camera"_s;
    case Kind::Microphone:
        return "microphone"_s;
    }
    return "notifications"_s;
}

void SitePermissionsHaiku::setListener(const BMessenger& listener)
{
    ASSERT(RunLoop::isMain());
    m_listener = listener;
    if (!listener.IsValid()) {
        auto pending = std::exchange(m_pending, { });
        for (auto& request : pending.values()) {
            if (request.completion)
                request.completion(false);
            if (request.mediaCompletion)
                request.mediaCompletion(false, { });
        }
    }
}

bool SitePermissionsHaiku::post(BMessage& message) const
{
    return !m_shutdown && m_listener.IsValid() && m_listener.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) == B_OK;
}

void SitePermissionsHaiku::setDecision(Kind kind, const String& origin, std::optional<bool> allowed)
{
    ASSERT(RunLoop::isMain());
    if (origin.isEmpty())
        return;
    auto& decisions = m_decisions[static_cast<size_t>(kind)];
    if (allowed)
        decisions.set(origin, *allowed);
    else if (!decisions.remove(origin))
        return;
    updateWebProcesses(kind, origin, allowed);
}

std::optional<bool> SitePermissionsHaiku::decision(Kind kind, const String& origin) const
{
    auto& decisions = m_decisions[static_cast<size_t>(kind)];
    auto found = decisions.find(origin);
    if (found == decisions.end())
        return std::nullopt;
    return found->value;
}

HashMap<String, bool> SitePermissionsHaiku::decisions(Kind kind) const
{
    return m_decisions[static_cast<size_t>(kind)];
}

// Web processes cache the notification decisions they were given at launch
// and the answers they received; tell them all about a change.
void SitePermissionsHaiku::updateWebProcesses(Kind kind, const String& origin, std::optional<bool> allowed)
{
    if (kind != Kind::Notifications)
        return;
    RefPtr manager = m_processPool.supplement<WebNotificationManagerProxy>();
    if (!manager)
        return;
    Ref apiOrigin = API::SecurityOrigin::createFromString(origin);
    if (allowed)
        manager->providerDidUpdateNotificationPolicy(apiOrigin.ptr(), *allowed);
    else {
        Ref origins = API::Array::create({ RefPtr<API::Object> { WTF::move(apiOrigin) } });
        manager->providerDidRemoveNotificationPolicies(origins.ptr());
    }
}

void SitePermissionsHaiku::request(Kind kind, const String& origin, const BMessenger& view, CompletionHandler<void(bool)>&& completion)
{
    ASSERT(RunLoop::isMain());
    if (auto saved = decision(kind, origin)) {
        completion(*saved);
        return;
    }
    if (m_shutdown || !m_listener.IsValid() || origin.isEmpty() || origin == "null"_s) {
        completion(false);
        return;
    }
    const uint64_t identifier = ++m_lastRequest;
    BMessage message(B_WEBKIT_PERMISSION_REQUESTED);
    message.AddUInt64("identifier", identifier);
    message.AddString("permission", nameForKind(kind).characters());
    message.AddString("origin", origin.utf8().legacyCStringPointer());
    message.AddBool("private", m_private);
    if (view.IsValid())
        message.AddMessenger("view", view);
    m_pending.add(identifier, Pending { kind, origin, WTF::move(completion) });
    if (!post(message)) {
        if (auto request = m_pending.take(identifier); request.completion)
            request.completion(false);
    }
}

void SitePermissionsHaiku::respond(uint64_t identifier, bool allowed, const String& deviceId)
{
    ASSERT(RunLoop::isMain());
    auto request = m_pending.take(identifier);
    if (request.completion)
        request.completion(allowed);
    if (request.mediaCompletion)
        request.mediaCompletion(allowed, deviceId);
}

void SitePermissionsHaiku::requestMedia(const String& origin, const BMessenger& view, MediaRequest&& media, MediaCompletion&& completion)
{
    ASSERT(RunLoop::isMain());
    if (!media.screen) {
        auto camera = media.camera ? decision(Kind::Camera, origin) : std::optional<bool> { true };
        auto microphone = media.microphone ? decision(Kind::Microphone, origin) : std::optional<bool> { true };
        if ((camera && !*camera) || (microphone && !*microphone)) {
            completion(false, { });
            return;
        }
        if (camera && microphone) {
            completion(true, { });
            return;
        }
    }
    if (m_shutdown || !m_listener.IsValid() || origin.isEmpty() || origin == "null"_s || (media.screen && media.screens.isEmpty())) {
        completion(false, { });
        return;
    }

    ASCIILiteral permission = media.screen ? "screen"_s
        : media.camera && media.microphone ? "camera-microphone"_s
        : media.camera ? "camera"_s : "microphone"_s;
    const uint64_t identifier = ++m_lastRequest;
    BMessage message(B_WEBKIT_PERMISSION_REQUESTED);
    message.AddUInt64("identifier", identifier);
    message.AddString("permission", permission.characters());
    message.AddString("origin", origin.utf8().legacyCStringPointer());
    message.AddBool("private", m_private);
    if (media.screen) {
        message.AddBool("audio", media.screenAudio);
        for (auto& [id, label] : media.screens) {
            message.AddString("device_id", id.utf8().legacyCStringPointer());
            message.AddString("device_label", label.utf8().legacyCStringPointer());
        }
    }
    if (view.IsValid())
        message.AddMessenger("view", view);
    m_pending.add(identifier, Pending { media.camera ? Kind::Camera : Kind::Microphone, origin, { }, WTF::move(completion) });
    if (!post(message)) {
        if (auto request = m_pending.take(identifier); request.mediaCompletion)
            request.mediaCompletion(false, { });
    }
}

std::optional<WebCore::PermissionState> SitePermissionsHaiku::query(Kind kind, const String& origin) const
{
    if (auto saved = decision(kind, origin))
        return *saved ? WebCore::PermissionState::Granted : WebCore::PermissionState::Denied;
    return WebCore::PermissionState::Prompt;
}

bool SitePermissionsHaiku::showNotification(WebPageProxy* page, WebNotification& notification)
{
    ASSERT(RunLoop::isMain());
    if (m_shutdown || !m_listener.IsValid())
        return false;
    const uint64_t identifier = notification.identifier().toUInt64();
    BMessage message(B_WEBKIT_NOTIFICATION_SHOW);
    message.AddUInt64("identifier", identifier);
    message.AddString("title", notification.title().utf8().legacyCStringPointer());
    message.AddString("body", notification.body().utf8().legacyCStringPointer());
    message.AddString("tag", notification.tag().utf8().legacyCStringPointer());
    message.AddString("icon_url", notification.iconURL().utf8().legacyCStringPointer());
    message.AddString("lang", notification.lang().utf8().legacyCStringPointer());
    message.AddString("origin", notification.origin().securityOrigin().toString().utf8().legacyCStringPointer());
    message.AddBool("rtl", notification.dir() == WebCore::NotificationDirection::Rtl);
    message.AddBool("silent", notification.data().silent.value_or(false));
    if (page) {
        if (auto* view = static_cast<WebView*>(page->pageClient())) {
            if (auto native = view->nativeView(); native.IsValid())
                message.AddMessenger("view", native);
        }
    }
    if (!post(message))
        return false;
    m_notifications.set(identifier, notification.identifier());
    if (RefPtr manager = m_processPool.supplement<WebNotificationManagerProxy>())
        manager->providerDidShowNotification(notification.identifier());
    return true;
}

void SitePermissionsHaiku::cancelNotification(WebNotification& notification)
{
    const uint64_t identifier = notification.identifier().toUInt64();
    if (!m_notifications.remove(identifier))
        return;
    BMessage message(B_WEBKIT_NOTIFICATION_CLOSE);
    message.AddUInt64("identifier", identifier);
    post(message);
}

void SitePermissionsHaiku::forgetNotification(WebNotification& notification)
{
    m_notifications.remove(notification.identifier().toUInt64());
}

void SitePermissionsHaiku::notificationClicked(uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    auto found = m_notifications.find(identifier);
    if (found == m_notifications.end())
        return;
    if (RefPtr manager = m_processPool.supplement<WebNotificationManagerProxy>())
        manager->providerDidClickNotification(found->value);
}

void SitePermissionsHaiku::notificationClosed(uint64_t identifier)
{
    ASSERT(RunLoop::isMain());
    auto found = m_notifications.find(identifier);
    if (found == m_notifications.end())
        return;
    const uint64_t raw = found->value.toUInt64();
    m_notifications.remove(found);
    if (RefPtr manager = m_processPool.supplement<WebNotificationManagerProxy>()) {
        Ref identifiers = API::Array::create({ RefPtr<API::Object> { API::UInt64::create(raw) } });
        manager->providerDidCloseNotifications(identifiers.ptr());
    }
}

void SitePermissionsHaiku::geolocationStart(WebGeolocationManagerProxy& manager)
{
    ASSERT(RunLoop::isMain());
    m_geolocation = &manager;
    BMessage message(B_WEBKIT_GEOLOCATION_START);
    message.AddBool("high_accuracy", m_highAccuracy);
    if (!post(message)) {
        m_geolocation = nullptr;
        manager.providerDidFailToDeterminePosition("Location is not available"_s);
    }
}

void SitePermissionsHaiku::geolocationStop()
{
    ASSERT(RunLoop::isMain());
    if (!m_geolocation)
        return;
    m_geolocation = nullptr;
    BMessage message(B_WEBKIT_GEOLOCATION_STOP);
    post(message);
}

void SitePermissionsHaiku::geolocationAccuracy(bool high)
{
    m_highAccuracy = high;
    if (!m_geolocation)
        return;
    BMessage message(B_WEBKIT_GEOLOCATION_START);
    message.AddBool("high_accuracy", high);
    post(message);
}

void SitePermissionsHaiku::setPosition([[maybe_unused]] double latitude, [[maybe_unused]] double longitude, [[maybe_unused]] double accuracy, [[maybe_unused]] double timestamp)
{
    ASSERT(RunLoop::isMain());
#if ENABLE(GEOLOCATION)
    if (!m_geolocation)
        return;
    Ref position = WebGeolocationPosition::create(WebCore::GeolocationPositionData(timestamp, latitude, longitude, accuracy));
    m_geolocation->providerDidChangePosition(position.ptr());
#endif
}

void SitePermissionsHaiku::positionUnavailable(const String& message)
{
    ASSERT(RunLoop::isMain());
    if (m_geolocation)
        m_geolocation->providerDidFailToDeterminePosition(message);
}

} // namespace WebKit
