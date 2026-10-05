/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionMatchPattern.h"
#include <wtf/URL.h>

namespace WebKit {

// One instance belongs to one extension's tab. This is never a global host
// permission and must not be added to shared content-script or CORS lists.
class WebExtensionActiveTabGrant {
public:
    bool grant(const URL& committedOrigin, bool hasFileAccess);
    void clear();
    bool isGranted() const { return !!m_pattern; }
    RefPtr<WebExtensionMatchPattern> pattern() const { return m_pattern; }
    bool matchesURL(const URL& committedOrigin, const URL& requestedURL, bool hasFileAccess) const;
    bool matchesAnyPattern(const WebExtensionMatchPattern::MatchPatternSet&) const;
    bool revokeForNavigation(const URL& committedOrigin);
private:
    bool sameOrigin(const URL&) const;
    URL m_origin;
    RefPtr<WebExtensionMatchPattern> m_pattern;
};

} // namespace WebKit
#endif
