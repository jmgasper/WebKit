/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WKBase.h"
#include <Messenger.h>
#include <memory>
#include <string>
#include <vector>

class BWebKitView;

enum : uint32 {
    // Messages of an inspector session wait to be taken. Sent when a message
    // is queued and the last announcement has been answered with Take(), so
    // a busy page costs the target one message, not thousands. Fields:
    // "identifier" (uint64, as given to Create()).
    B_WEBKIT_INSPECTOR_MESSAGES = 'winm'
};

// A frontend of the Web Inspector protocol, connected to the page of a view:
// what a host builds its developer tools on. Messages are the protocol's JSON
// text (Source/JavaScriptCore/inspector/protocol), in UTF-8.
//
// The session speaks to the page's target router, as a remote Web Inspector
// does: the page and its agents (Network, Console, Page, Runtime...) are a
// target that "Target.targetCreated" announces, reached with
// "Target.sendMessageToTarget" and heard through
// "Target.dispatchMessageFromTarget". A navigation that changes process
// replaces the target ("Target.didCommitProvisionalTarget"); the session
// stays connected.
//
// Connecting turns on the developer extras of the page's preferences, which
// console messages need to reach an inspector; they are turned off when the
// last session of those preferences ends. The session ends when it is
// destroyed or when the page closes; messages sent after that are dropped.
class WK_EXPORT BWebKitInspectorSession {
public:
    // Any thread. The view must be alive during the call only.
    static std::shared_ptr<BWebKitInspectorSession> Create(BWebKitView&, BMessenger target, uint64 identifier = 0);
    ~BWebKitInspectorSession();
    // Queues a protocol message for the page. Any thread.
    void Send(const char* message);
    // Appends the messages that arrived since the last call, in order, and
    // returns whether there were any. Any thread.
    bool Take(std::vector<std::string>& messages);
    // Messages were dropped because more than 256 MiB waited to be taken.
    bool Overflowed() const;

    struct State;

private:
    explicit BWebKitInspectorSession(std::shared_ptr<State>);
    BWebKitInspectorSession(const BWebKitInspectorSession&) = delete;
    BWebKitInspectorSession& operator=(const BWebKitInspectorSession&) = delete;
    std::shared_ptr<State> m_state;
};
