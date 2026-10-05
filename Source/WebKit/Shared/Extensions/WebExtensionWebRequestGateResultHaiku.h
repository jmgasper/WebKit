/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <optional>
#include <utility>
#include <wtf/Seconds.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

// Blocking webRequest phases, in network order. The values travel over IPC as
// uint8_t and must stay in sync with WebExtensionWebRequestPhaseHaiku.
constexpr uint8_t webExtensionWebRequestBeforeRequestPhaseHaiku = 0;
constexpr uint8_t webExtensionWebRequestBeforeSendHeadersPhaseHaiku = 1;
constexpr uint8_t webExtensionWebRequestHeadersReceivedPhaseHaiku = 2;
constexpr uint8_t webExtensionWebRequestAuthRequiredPhaseHaiku = 3;
constexpr uint8_t webExtensionWebRequestLastPhaseHaiku = webExtensionWebRequestAuthRequiredPhaseHaiku;

constexpr uint8_t webExtensionWebRequestPhaseBitHaiku(uint8_t phase) { return static_cast<uint8_t>(1u << phase); }
constexpr uint8_t webExtensionWebRequestAllPhasesHaiku = 0x0f;

// A blocking decision never holds a load for longer than this. The UI process
// completes the network process' request unmodified (fail open) when it fires.
// The WebProcess stops waiting for Promise/asyncBlocking listeners slightly
// earlier so that late listeners do not keep their transaction alive.
constexpr Seconds webExtensionWebRequestBlockingTimeoutHaiku { 20_s };
constexpr Seconds webExtensionWebRequestListenerTimeoutHaiku { 18_s };

// Extension-supplied credentials are offered at most this many times for one
// load before the ordinary authentication path takes over, which bounds loops
// caused by wrong credentials.
constexpr unsigned webExtensionWebRequestMaximumCredentialAttemptsHaiku = 3;

// The merged, authorized result for one phase of one request. An empty result
// continues the load unmodified. headers carries request headers for
// BeforeSendHeaders and response headers for HeadersReceived.
struct WebExtensionWebRequestGateResultHaiku {
    bool cancel { false };
    std::optional<URL> redirectURL;
    std::optional<Vector<std::pair<String, String>>> headers;
    std::optional<std::pair<String, String>> credentials;

    bool isEmpty() const { return !cancel && !redirectURL && !headers && !credentials; }
};

}
#endif
