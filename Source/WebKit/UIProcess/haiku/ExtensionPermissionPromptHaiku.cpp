/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "ExtensionPermissionPromptHaiku.h"

#include "WebKitExtensionPermission.h"
#include <algorithm>
#include <wtf/UUID.h>

namespace WebKit {

std::shared_ptr<ExtensionPermissionPromptHaiku> ExtensionPermissionPromptHaiku::create(bigtime_t timeout)
{
    ASSERT(RunLoop::isMain());
    return std::shared_ptr<ExtensionPermissionPromptHaiku>(new ExtensionPermissionPromptHaiku(timeout));
}

ExtensionPermissionPromptHaiku::ExtensionPermissionPromptHaiku(bigtime_t timeout)
    : m_timer(RunLoop::mainSingleton(), "ExtensionPermissionPromptHaiku"_s, [this] { pump(); })
    , m_timeout(std::clamp<bigtime_t>(timeout, 100000, 120000000))
{
}

ExtensionPermissionPromptHaiku::~ExtensionPermissionPromptHaiku()
{
    shutdown();
}

status_t ExtensionPermissionPromptHaiku::setListener(const BMessenger& listener)
{
    ASSERT(RunLoop::isMain());
    if (m_shutdown)
        return B_CANCELED;
    if (!m_requests.empty())
        return B_BUSY;
    // Consent is a trusted application UI operation, never an external team.
    if (listener.IsValid() && !listener.IsTargetLocal())
        return B_NOT_ALLOWED;
    m_listener = listener;
    return B_OK;
}

void ExtensionPermissionPromptHaiku::dispatch(Completion&& completion, std::expected<bool, status_t> result)
{
    RunLoop::mainSingleton().dispatch([completion = WTF::move(completion), result]() mutable {
        completion(result);
    });
}

String ExtensionPermissionPromptHaiku::request(uint64_t owner, Details&& details, Completion&& completion)
{
    ASSERT(RunLoop::isMain());
    status_t error = B_OK;
    if (m_shutdown)
        error = B_CANCELED;
    else if (!m_listener.IsValid())
        error = B_NO_INIT;
    else if (m_requests.size() >= 32)
        error = B_BUSY;
    else if (!owner || details.extensionIdentifier.isEmpty() || details.extensionName.isEmpty()
        || details.permissions.size() + details.origins.size() > 256
        || (details.permissions.isEmpty() && details.origins.isEmpty()))
        error = B_BAD_VALUE;
    if (error != B_OK) {
        dispatch(WTF::move(completion), std::unexpected(error));
        return { };
    }

    size_t length = 0;
    auto validate = [&](const String& text) {
        if (text.isEmpty() || text.contains('\0') || text.length() > 8192)
            error = B_BAD_VALUE;
        length += text.length();
    };
    validate(details.extensionIdentifier);
    validate(details.extensionName);
    for (auto& permission : details.permissions)
        validate(permission);
    for (auto& origin : details.origins)
        validate(origin);
    if (length > 16384)
        error = B_BAD_VALUE;
    if (error != B_OK) {
        dispatch(WTF::move(completion), std::unexpected(error));
        return { };
    }

    auto identifier = WTF::UUID::createVersion4().toString();
    auto deadline = system_time() + m_timeout;
    BMessage message(B_WEBKIT_EXTENSION_PERMISSION_REQUESTED);
    auto add = [&](const char* field, const String& value) {
        if (error == B_OK)
            error = message.AddString(field, value.utf8().legacyCStringPointer());
    };
    add("identifier", identifier);
    add("extension_identifier", details.extensionIdentifier);
    add("extension_name", details.extensionName);
    for (auto& permission : details.permissions)
        add("permission", permission);
    for (auto& origin : details.origins)
        add("origin", origin);
    if (error == B_OK)
        error = message.AddInt64("deadline", deadline);
    if (error == B_OK)
        error = message.AddBool("private_browsing", details.privateBrowsing);
    if (error != B_OK) {
        dispatch(WTF::move(completion), std::unexpected(error));
        return { };
    }
    m_requests.push_back(std::make_unique<Request>(Request { owner, identifier, message, WTF::move(completion), deadline }));
    pump();
    return identifier;
}

void ExtensionPermissionPromptHaiku::complete(std::unique_ptr<Request> request, std::expected<bool, status_t> result, bool dismiss)
{
    if (dismiss && request->delivered) {
        BMessage message(B_WEBKIT_EXTENSION_PERMISSION_CANCELLED);
        if (message.AddString("identifier", request->identifier.utf8().legacyCStringPointer()) == B_OK)
            m_listener.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
        // A host also receives the absolute deadline; queue congestion cannot
        // make a stale prompt live indefinitely. Its response is already inert.
    }
    dispatch(WTF::move(request->completion), result);
}

void ExtensionPermissionPromptHaiku::pump()
{
    ASSERT(RunLoop::isMain());
    auto now = system_time();
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (now >= (*it)->deadline || !m_listener.IsValid()) {
            auto request = std::move(*it);
            it = m_requests.erase(it);
            auto error = now >= request->deadline ? B_TIMED_OUT : B_BAD_PORT_ID;
            complete(std::move(request), std::unexpected(error), true);
        } else
            ++it;
    }
    while (!m_requests.empty() && !m_requests.front()->delivered) {
        auto& request = *m_requests.front();
        if (!request.deliveryDeadline)
            request.deliveryDeadline = std::min(now + 1000000, request.deadline);
        auto error = m_listener.SendMessage(&request.message, static_cast<BHandler*>(nullptr), 0);
        if (error == B_OK) {
            request.delivered = true;
            break;
        }
        if ((error == B_WOULD_BLOCK || error == B_TIMED_OUT) && now < request.deliveryDeadline)
            break;
        auto failed = std::move(m_requests.front());
        m_requests.pop_front();
        complete(std::move(failed), std::unexpected(error), false);
    }
    if (m_requests.empty())
        m_timer.stop();
    else if (!m_timer.isActive())
        m_timer.startRepeating(100_ms);
}

void ExtensionPermissionPromptHaiku::respond(const String& identifier, bool allowed)
{
    ASSERT(RunLoop::isMain());
    if (m_requests.empty() || !m_requests.front()->delivered || m_requests.front()->identifier != identifier)
        return;
    auto request = std::move(m_requests.front());
    m_requests.pop_front();
    auto result = system_time() < request->deadline ? std::expected<bool, status_t>(allowed) : std::unexpected(B_TIMED_OUT);
    complete(std::move(request), result, true);
    pump();
}

void ExtensionPermissionPromptHaiku::cancelOwner(uint64_t owner)
{
    ASSERT(RunLoop::isMain());
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if ((*it)->owner == owner) {
            auto request = std::move(*it);
            it = m_requests.erase(it);
            complete(std::move(request), std::unexpected(B_CANCELED), true);
        } else
            ++it;
    }
    pump();
}

void ExtensionPermissionPromptHaiku::cancelAll()
{
    ASSERT(RunLoop::isMain());
    auto requests = std::exchange(m_requests, { });
    m_timer.stop();
    for (auto& request : requests)
        complete(std::move(request), std::unexpected(B_CANCELED), true);
}

void ExtensionPermissionPromptHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    m_shutdown = true;
    cancelAll();
    m_listener = { };
}

bool ExtensionPermissionPromptHaiku::hasPendingRequests() const
{
    ASSERT(RunLoop::isMain());
    return !m_requests.empty();
}

} // namespace WebKit
