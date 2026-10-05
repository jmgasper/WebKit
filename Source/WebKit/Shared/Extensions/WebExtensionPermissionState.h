/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <expected>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/JSONValues.h>
#include <wtf/WallTime.h>

namespace WebKit {

struct WebExtensionPermissionDetails {
    HashSet<String> permissions;
    HashSet<String> origins;
};

struct WebExtensionPermissionState {
    using Grants = HashMap<String, WallTime>;
    Grants permissions;
    Grants origins;
};

// Parse structure only. Both processes must separately validate permission
// names, match patterns and manifest declarations for the requested operation.
std::expected<WebExtensionPermissionDetails, String> parseWebExtensionPermissionDetails(const JSON::Value*);

// Saved data cannot grant unsupported, undeclared or denied capabilities:
// the context must filter the decoded sets against its current manifest and
// host policy before installing them. Malformed state fails closed.
std::expected<WebExtensionPermissionState, String> readWebExtensionPermissionState(const JSON::Object*, WallTime now);
// Prepare a deep copy, retaining unrelated extension settings. Infinity is
// encoded as null; finite expirations are epoch seconds. No memory/disk mutation.
std::expected<Ref<JSON::Object>, String> stateWithWebExtensionPermissions(const JSON::Object*, const WebExtensionPermissionState&);

} // namespace WebKit

#endif
