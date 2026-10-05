/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionActiveTabGrant.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
namespace WebKit {

bool WebExtensionActiveTabGrant::grant(const URL& origin, bool hasFileAccess)
{
    if (!origin.isValid() || (!origin.protocolIsInHTTPFamily() && !(origin.protocolIsFile() && hasFileAccess))) {
        clear();
        return false;
    }
    // The default match-pattern constructor includes subdomains and both HTTP
    // schemes. A user invocation grants only the main frame's exact origin.
    auto pattern = WebExtensionMatchPattern::getOrCreate(origin, { WebExtensionMatchPattern::CreateOptions::MatchExactScheme, WebExtensionMatchPattern::CreateOptions::MatchAllPaths });
    if (!pattern || !pattern->isSupported()) {
        clear();
        return false;
    }
    m_origin = origin;
    m_pattern = WTF::move(pattern);
    return true;
}

void WebExtensionActiveTabGrant::clear()
{
    m_pattern = nullptr;
    m_origin = { };
}

bool WebExtensionActiveTabGrant::sameOrigin(const URL& origin) const
{
    return m_pattern && origin.isValid() && protocolHostAndPortAreEqual(m_origin, origin);
}

bool WebExtensionActiveTabGrant::matchesURL(const URL& currentOrigin, const URL& requestedURL, bool hasFileAccess) const
{
    return sameOrigin(currentOrigin) && sameOrigin(requestedURL)
        && (!m_origin.protocolIsFile() || hasFileAccess);
}

bool WebExtensionActiveTabGrant::matchesAnyPattern(const WebExtensionMatchPattern::MatchPatternSet& patterns) const
{
    if (!m_pattern)
        return false;
    for (auto& pattern : patterns) {
        if (pattern->matchesURL(m_origin, { WebExtensionMatchPattern::Options::IgnorePaths, WebExtensionMatchPattern::Options::AllowFileScheme }))
            return true;
    }
    return false;
}

bool WebExtensionActiveTabGrant::revokeForNavigation(const URL& origin)
{
    if (!m_pattern || sameOrigin(origin))
        return false;
    clear();
    return true;
}

} // namespace WebKit
#endif
