/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionCookieParameters.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
struct WebExtensionCookieWrite {
    std::optional<PAL::SessionID> session;
    URL url;
    WebCore::Cookie cookie;
};
// Both processes parse the original details. This validates cookie structure
// and origin/domain relationships; it never grants access to a host or store.
std::expected<WebExtensionCookieWrite, String> parseWebExtensionCookieWrite(const JSON::Value*);
}
#endif
