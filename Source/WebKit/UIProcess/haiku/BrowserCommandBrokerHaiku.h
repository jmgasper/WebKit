/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WebPageProxyIdentifier.h"
#include <Message.h>
#include <Messenger.h>
#include <memory>
#include <wtf/CompletionHandler.h>
#include <wtf/RunLoop.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

// Main RunLoop only. Extensions ask the host browser to change its own tab and
// window model through this broker; the engine never opens, closes, selects or
// navigates a host tab by itself. Every request completes exactly once and
// always asynchronously: with the host's answer, or with an error when there is
// no listener, delivery fails, the deadline passes or the owner is cancelled.
class BrowserCommandBrokerHaiku final {
public:
    struct Result {
        status_t status { B_ERROR };
        // Optional host-supplied explanation for a failure.
        String error;
        // Pages of the views the host created for this command, in host order.
        Vector<WebPageProxyIdentifier> createdPages;
    };
    using Completion = CompletionHandler<void(Result&&)>;

    static std::shared_ptr<BrowserCommandBrokerHaiku> create(bigtime_t timeout = 30000000);
    ~BrowserCommandBrokerHaiku();

    // Only local targets are accepted. Replacing or clearing the listener
    // fails the requests that were addressed to the previous one.
    status_t setListener(const BMessenger&);
    bool hasListener() const { return m_listener.IsValid(); }

    // owner identifies one extension load and is never sent to the host. The
    // broker adds identifier and deadline to the message before delivery.
    // Returns the request identifier, or zero when it failed immediately.
    uint64_t request(uint64_t owner, BMessage&&, Completion&&);
    // Stale, duplicate and unknown identifiers are ignored.
    void respond(uint64_t identifier, Result&&);
    void cancelOwner(uint64_t owner);
    void cancelAll();
    void shutdown();
    bool hasPendingRequests() const { return !m_requests.isEmpty(); }

private:
    explicit BrowserCommandBrokerHaiku(bigtime_t);
    struct Request {
        uint64_t owner;
        uint64_t identifier;
        BMessage message;
        Completion completion;
        bigtime_t deadline;
        bigtime_t deliveryDeadline { 0 };
        bool delivered { false };
    };
    void pump();
    void complete(std::unique_ptr<Request>, Result&&, bool notifyHost);
    static void dispatch(Completion&&, Result&&);

    BMessenger m_listener;
    Vector<std::unique_ptr<Request>> m_requests;
    RunLoop::Timer m_timer;
    const bigtime_t m_timeout;
    uint64_t m_nextIdentifier { 1 };
    bool m_shutdown { false };
};

} // namespace WebKit
