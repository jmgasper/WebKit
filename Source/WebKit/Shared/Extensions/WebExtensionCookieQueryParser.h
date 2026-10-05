/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionCookieParameters.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
enum class WebExtensionCookieQueryKind : uint8_t { Get, GetAll };
struct WebExtensionCookieQuery {
    std::optional<PAL::SessionID> session;
    URL url;
    WebExtensionCookieFilterParameters filter;
};
// This is structural validation. It never resolves a store or grants access.
std::expected<WebExtensionCookieQuery, String> parseWebExtensionCookieQuery(const JSON::Value*, WebExtensionCookieQueryKind);
}
#endif
