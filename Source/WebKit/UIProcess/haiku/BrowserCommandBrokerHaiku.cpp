/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "BrowserCommandBrokerHaiku.h"

#include "WebKitContext.h"
#include <algorithm>
#include <limits>

namespace WebKit {

std::shared_ptr<BrowserCommandBrokerHaiku> BrowserCommandBrokerHaiku::create(bigtime_t timeout)
{
    ASSERT(RunLoop::isMain());
    return std::shared_ptr<BrowserCommandBrokerHaiku>(new BrowserCommandBrokerHaiku(timeout));
}

BrowserCommandBrokerHaiku::BrowserCommandBrokerHaiku(bigtime_t timeout)
    : m_timer(RunLoop::mainSingleton(), "BrowserCommandBrokerHaiku"_s, [this] { pump(); })
    , m_timeout(std::clamp<bigtime_t>(timeout, 100000, 120000000))
{
}

BrowserCommandBrokerHaiku::~BrowserCommandBrokerHaiku()
{
    shutdown();
}

status_t BrowserCommandBrokerHaiku::setListener(const BMessenger& listener)
{
    ASSERT(RunLoop::isMain());
    if (m_shutdown)
        return B_CANCELED;
    // Tab and window changes are trusted browser UI, never another team.
    if (listener.IsValid() && !listener.IsTargetLocal())
        return B_NOT_ALLOWED;
    if (listener == m_listener)
        return B_OK;
    // The previous host can no longer answer; do not leave its callers waiting
    // for the deadline, and never replay a command to a different listener.
    auto requests = std::exchange(m_requests, { });
    for (auto& request : requests)
        complete(WTF::move(request), { B_CANCELED, { }, { } }, true);
    m_listener = listener;
    pump();
    return B_OK;
}

void BrowserCommandBrokerHaiku::dispatch(Completion&& completion, Result&& result)
{
    RunLoop::mainSingleton().dispatch([completion = WTF::move(completion), result = WTF::move(result)]() mutable {
        completion(WTF::move(result));
    });
}

uint64_t BrowserCommandBrokerHaiku::request(uint64_t owner, BMessage&& message, Completion&& completion)
{
    ASSERT(RunLoop::isMain());
    status_t error = B_OK;
    if (m_shutdown)
        error = B_CANCELED;
    else if (!m_listener.IsValid())
        error = B_NO_INIT;
    else if (m_requests.size() >= 256 || m_nextIdentifier == std::numeric_limits<uint64_t>::max())
        error = B_BUSY;
    else if (!owner || message.what != B_WEBKIT_BROWSER_COMMAND || message.HasUInt64("identifier") || message.HasInt64("deadline"))
        error = B_BAD_VALUE;
    if (error != B_OK) {
        dispatch(WTF::move(completion), { error, { }, { } });
        return 0;
    }

    auto identifier = m_nextIdentifier++;
    auto deadline = system_time() + m_timeout;
    error = message.AddUInt64("identifier", identifier);
    if (error == B_OK)
        error = message.AddInt64("deadline", deadline);
    if (error != B_OK) {
        dispatch(WTF::move(completion), { error, { }, { } });
        return 0;
    }
    m_requests.append(std::make_unique<Request>(Request { owner, identifier, WTF::move(message), WTF::move(completion), deadline }));
    pump();
    return identifier;
}

void BrowserCommandBrokerHaiku::complete(std::unique_ptr<Request> request, Result&& result, bool notifyHost)
{
    if (notifyHost && request->delivered && m_listener.IsValid()) {
        // Advisory only: the host also received the absolute deadline, and its
        // late response is already inert because the identifier is forgotten.
        BMessage message(B_WEBKIT_BROWSER_COMMAND_CANCELLED);
        if (message.AddUInt64("identifier", request->identifier) == B_OK)
            m_listener.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }
    dispatch(WTF::move(request->completion), WTF::move(result));
}

void BrowserCommandBrokerHaiku::pump()
{
    ASSERT(RunLoop::isMain());
    auto now = system_time();
    Vector<std::unique_ptr<Request>> finished;
    Vector<status_t> errors;
    for (size_t index = 0; index < m_requests.size();) {
        auto& request = *m_requests[index];
        status_t error = B_OK;
        if (now >= request.deadline)
            error = B_TIMED_OUT;
        else if (!m_listener.IsValid())
            error = B_BAD_PORT_ID;
        else if (!request.delivered) {
            if (!request.deliveryDeadline)
                request.deliveryDeadline = std::min(now + 1000000, request.deadline);
            // Never block the WebKit main thread on a busy host looper.
            auto status = m_listener.SendMessage(&request.message, static_cast<BHandler*>(nullptr), 0);
            if (status == B_OK)
                request.delivered = true;
            else if ((status != B_WOULD_BLOCK && status != B_TIMED_OUT) || now >= request.deliveryDeadline)
                error = status;
        }
        if (error == B_OK) {
            ++index;
            continue;
        }
        finished.append(WTF::move(m_requests[index]));
        errors.append(error);
        m_requests.removeAt(index);
    }
    for (size_t index = 0; index < finished.size(); ++index)
        complete(WTF::move(finished[index]), { errors[index], { }, { } }, errors[index] == B_TIMED_OUT);
    if (m_requests.isEmpty())
        m_timer.stop();
    else if (!m_timer.isActive())
        m_timer.startRepeating(100_ms);
}

void BrowserCommandBrokerHaiku::respond(uint64_t identifier, Result&& result)
{
    ASSERT(RunLoop::isMain());
    auto index = m_requests.findIf([&](auto& request) { return request->identifier == identifier; });
    if (index == notFound || !m_requests[index]->delivered)
        return;
    auto request = WTF::move(m_requests[index]);
    m_requests.removeAt(index);
    if (system_time() >= request->deadline)
        result = { B_TIMED_OUT, { }, { } };
    complete(WTF::move(request), WTF::move(result), false);
    pump();
}

void BrowserCommandBrokerHaiku::cancelOwner(uint64_t owner)
{
    ASSERT(RunLoop::isMain());
    Vector<std::unique_ptr<Request>> cancelled;
    for (size_t index = 0; index < m_requests.size();) {
        if (m_requests[index]->owner != owner) {
            ++index;
            continue;
        }
        cancelled.append(WTF::move(m_requests[index]));
        m_requests.removeAt(index);
    }
    for (auto& request : cancelled)
        complete(WTF::move(request), { B_CANCELED, { }, { } }, true);
    pump();
}

void BrowserCommandBrokerHaiku::cancelAll()
{
    ASSERT(RunLoop::isMain());
    auto requests = std::exchange(m_requests, { });
    m_timer.stop();
    for (auto& request : requests)
        complete(WTF::move(request), { B_CANCELED, { }, { } }, true);
}

void BrowserCommandBrokerHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    m_shutdown = true;
    cancelAll();
    m_listener = BMessenger();
}

} // namespace WebKit
