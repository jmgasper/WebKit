/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionWebRequestDecisionHaiku.h"
#include <wtf/HashSet.h>
#include <wtf/text/StringHash.h>

namespace WebKit {

enum class WebExtensionWebRequestPhaseHaiku : uint8_t { BeforeRequest, BeforeSendHeaders, HeadersReceived, AuthRequired };
enum class WebExtensionWebRequestIgnoredActionHaiku : uint8_t { Redirect, RequestHeaders, ResponseHeaders, Credentials };
enum class WebExtensionWebRequestHeaderOperationHaiku : uint8_t { Remove, Set, Append };

struct WebExtensionWebRequestHeaderConstraintHaiku {
    String name;
    WebExtensionWebRequestHeaderOperationHaiku operation;
};

struct WebExtensionWebRequestReplyHaiku {
    // All fields except decision come from the retained UI dispatch record,
    // never from the extension's response or a WebProcess-supplied identity.
    String installationIdentifier;
    uint64_t installationOrder { 0 };
    uint64_t listenerOrder { 0 };
    HashSet<String> hiddenHeaderNames; // Lowercase names excluded by this listener's access policy.
    WebExtensionWebRequestDecisionHaiku decision;
};

struct WebExtensionWebRequestIgnoredReplyHaiku {
    String installationIdentifier;
    uint64_t listenerOrder;
    WebExtensionWebRequestIgnoredActionHaiku action;
};

struct WebExtensionWebRequestMergedReplyHaiku {
    bool cancel { false };
    String cancelingInstallation;
    std::optional<URL> redirectURL;
    String redirectingInstallation;
    std::optional<Vector<WebExtensionWebRequestHeaderHaiku>> headers;
    std::optional<WebExtensionWebRequestCredentialsHaiku> authCredentials;
    Vector<WebExtensionWebRequestIgnoredReplyHaiku> ignored;
};

// Resolve already decoded, currently authorized replies against the immutable
// headers each listener saw (before any listener's edits). The UI must recheck
// lifetime, generation, host/private permissions and redirect resource access
// before calling this and again before applying the result. DNR constraints
// describe modifications already applied to originalHeaders by the rule engine.
// This function neither dispatches callbacks nor authorizes network operations.
std::expected<WebExtensionWebRequestMergedReplyHaiku, String> mergeWebExtensionWebRequestRepliesHaiku(
    WebExtensionWebRequestPhaseHaiku, const URL& originalURL,
    const Vector<WebExtensionWebRequestHeaderHaiku>& originalHeaders,
    const Vector<WebExtensionWebRequestHeaderConstraintHaiku>&,
    const Vector<WebExtensionWebRequestReplyHaiku>&);

}
#endif
