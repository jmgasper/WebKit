/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <Message.h>
#include <Messenger.h>
#include <deque>
#include <expected>
#include <memory>
#include <wtf/CompletionHandler.h>
#include <wtf/RunLoop.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

// Main RunLoop only. This broker collects consent; the extension context must
// revalidate its load, manifest and current grants before applying the result.
class ExtensionPermissionPromptHaiku final {
public:
    struct Details {
        String extensionIdentifier;
        String extensionName;
        Vector<String> permissions;
        Vector<String> origins;
        bool privateBrowsing { false };
    };
    using Completion = CompletionHandler<void(std::expected<bool, status_t>)>;

    static std::shared_ptr<ExtensionPermissionPromptHaiku> create(bigtime_t timeout = 120000000);
    ~ExtensionPermissionPromptHaiku();
    status_t setListener(const BMessenger&);
    // owner identifies a single extension load and is never sent to the host.
    // Every completion is asynchronous, including validation failures.
    String request(uint64_t owner, Details&&, Completion&&);
    void respond(const String& identifier, bool allowed);
    void cancelOwner(uint64_t owner);
    void cancelAll();
    void shutdown();
    bool hasPendingRequests() const;

private:
    explicit ExtensionPermissionPromptHaiku(bigtime_t);
    struct Request {
        uint64_t owner;
        String identifier;
        BMessage message;
        Completion completion;
        bigtime_t deadline;
        bigtime_t deliveryDeadline { 0 };
        bool delivered { false };
    };
    void pump();
    void complete(std::unique_ptr<Request>, std::expected<bool, status_t>, bool dismiss);
    static void dispatch(Completion&&, std::expected<bool, status_t>);

    BMessenger m_listener;
    std::deque<std::unique_ptr<Request>> m_requests;
    RunLoop::Timer m_timer;
    const bigtime_t m_timeout;
    bool m_shutdown { false };
};

} // namespace WebKit
