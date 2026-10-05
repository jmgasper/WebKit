/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <expected>
#include <optional>
#include <wtf/JSONValues.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>

namespace WebKit {

struct WebExtensionWebRequestHeaderHaiku {
    String name;
    Vector<uint8_t> value;
};

struct WebExtensionWebRequestCredentialsHaiku {
    String username;
    String password;
};

// Preserve absent header lists separately from an explicitly empty replacement,
// and preserve response-header duplicates and binary values without flattening.
struct WebExtensionWebRequestDecisionHaiku {
    bool cancel { false };
    std::optional<URL> redirectURL;
    std::optional<Vector<WebExtensionWebRequestHeaderHaiku>> requestHeaders;
    std::optional<Vector<WebExtensionWebRequestHeaderHaiku>> responseHeaders;
    std::optional<WebExtensionWebRequestCredentialsHaiku> authCredentials;
};

// This is the untrusted reply decoder, not an authorization decision. The UI
// must still verify the responding context, event phase, permissions, private
// access and redirect destination before any network mutation is permitted.
std::expected<WebExtensionWebRequestDecisionHaiku, String> parseWebExtensionWebRequestDecisionHaiku(const String&);

}
#endif
