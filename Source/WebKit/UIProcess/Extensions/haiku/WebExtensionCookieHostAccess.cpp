/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionCookieHostAccess.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
namespace WebKit {
bool hasWebExtensionCookieHostAccess(const URL& url, const HashMap<Ref<WebExtensionMatchPattern>, WallTime>& granted,
    const HashMap<Ref<WebExtensionMatchPattern>, WallTime>& denied, WallTime now)
{
    if (!url.isValid() || !url.protocolIsInHTTPFamily() || url.host().isEmpty())
        return false;
    // Preserve the context's explicit-host versus wildcard-host precedence.
    // Match paths are not part of a cookie host permission.
    for (bool allHosts : { false, true }) {
        for (auto& entry : denied) {
            if (entry.value > now && entry.key->matchesAllHosts() == allHosts
                && entry.key->matchesURL(url, WebExtensionMatchPattern::Options::IgnorePaths))
                return false;
        }
        for (auto& entry : granted) {
            if (entry.value > now && entry.key->matchesAllHosts() == allHosts
                && entry.key->matchesURL(url, WebExtensionMatchPattern::Options::IgnorePaths))
                return true;
        }
    }
    return false;
}
}
#endif
