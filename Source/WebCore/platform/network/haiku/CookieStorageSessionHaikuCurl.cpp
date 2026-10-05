/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CookieStorageSession.h"

#if PLATFORM(HAIKU) && USE(CURL)

#include "Cookie.h"
#include "CookieJarDB.h"
#include "CookieRequestHeaderFieldProxy.h"
#include "NotImplemented.h"
#include <wtf/text/StringBuilder.h>

namespace WebCore {

CookieJarDB& CookieStorageSession::cookieDatabase() const
{
    m_cookieDatabase->open();
    return *m_cookieDatabase;
}

// CookieJarDB does not store partition keys yet. A partitioned request must
// never fall through to the unpartitioned database.
static bool cookieAccessIsBlocked(ThirdPartyCookieBlockingDecision decision, const String& partition)
{
    return decision != ThirdPartyCookieBlockingDecision::None || !partition.isEmpty();
}

static std::pair<String, bool> cookiesForHaikuSession(const CookieStorageSession& session, const URL& firstParty, const URL& url, bool forHTTPHeader, IncludeSecureCookies includeSecureCookies)
{
    StringBuilder cookies;
    bool didAccessSecureCookies = false;
    auto searchHTTPOnly = forHTTPHeader ? std::nullopt : std::optional<bool> { false };
    auto secure = includeSecureCookies == IncludeSecureCookies::Yes ? std::nullopt : std::optional<bool> { false };

    if (auto result = session.cookieDatabase().searchCookies(firstParty, url, searchHTTPOnly, secure, std::nullopt)) {
        for (const auto& cookie : *result) {
            if (!cookies.isEmpty())
                cookies.append("; "_s);
            if (!cookie.name.isEmpty())
                cookies.append(cookie.name, '=');
            if (cookie.secure)
                didAccessSecureCookies = true;
            cookies.append(cookie.value);
        }
    }
    return { cookies.toString(), didAccessSecureCookies };
}

std::pair<String, bool> CookieStorageSession::cookiesForDOM(const URL& firstParty, const SameSiteInfo&, const URL& url, IncludeSecureCookies includeSecureCookies, ThirdPartyCookieBlockingDecision decision, const String& partition) const
{
    if (cookieAccessIsBlocked(decision, partition))
        return { };
    return cookiesForHaikuSession(*this, firstParty, url, false, includeSecureCookies);
}

std::pair<String, bool> CookieStorageSession::cookieRequestHeaderFieldValue(const URL& firstParty, const SameSiteInfo&, const URL& url, IncludeSecureCookies includeSecureCookies, ThirdPartyCookieBlockingDecision decision, const String& partition) const
{
    if (cookieAccessIsBlocked(decision, partition))
        return { };
    return cookiesForHaikuSession(*this, firstParty, url, true, includeSecureCookies);
}

std::pair<String, bool> CookieStorageSession::cookieRequestHeaderFieldValue(const CookieRequestHeaderFieldProxy& proxy, ThirdPartyCookieBlockingDecision decision, const String& partition) const
{
    return cookieRequestHeaderFieldValue(proxy.firstParty, proxy.sameSiteInfo, proxy.url, proxy.includeSecureCookies, decision, partition);
}

void CookieStorageSession::setCookiesFromDOM(const URL& firstParty, const SameSiteInfo&, const URL& url, const String& value, ThirdPartyCookieBlockingDecision decision, std::optional<Seconds> cappedLifetime, const String& partition) const
{
    if (!cookieAccessIsBlocked(decision, partition))
        cookieDatabase().setCookie(firstParty, url, value, CookieJarDB::Source::Script, cappedLifetime);
}

bool CookieStorageSession::setCookieFromDOM(const URL&, const SameSiteInfo&, const URL&, const Cookie&, ThirdPartyCookieBlockingDecision, std::optional<Seconds>, const String&) const
{
    // The inherited Curl Cookie Store API is not implemented. Keep reporting
    // failure until structured script writes enforce the same rules as strings.
    notImplemented();
    return false;
}

void CookieStorageSession::setCookiesFromHTTPResponse(const URL& firstParty, const URL& url, const String& value) const
{
    cookieDatabase().setCookie(firstParty, url, value, CookieJarDB::Source::Network);
}

bool CookieStorageSession::getRawCookies(const URL& firstParty, const SameSiteInfo&, const URL& url, ThirdPartyCookieBlockingDecision decision, const String& partition, Vector<Cookie>& rawCookies) const
{
    rawCookies.clear();
    if (cookieAccessIsBlocked(decision, partition))
        return true;
    auto cookies = cookieDatabase().searchCookies(firstParty, url, std::nullopt, std::nullopt, std::nullopt);
    if (!cookies)
        return false;
    rawCookies = WTF::move(*cookies);
    return true;
}

void CookieStorageSession::setCookie(const Cookie& cookie)
{
    if (cookie.partitionKey.isEmpty())
        cookieDatabase().setCookie(cookie);
}

void CookieStorageSession::deleteCookie(const URL&, const URL& url, const String& name, const String& partition, CompletionHandler<void()>&& completionHandler) const
{
    if (partition.isEmpty())
        cookieDatabase().deleteCookie(url.string(), name);
    completionHandler();
}

void CookieStorageSession::deleteAllCookies(CompletionHandler<void()>&& completionHandler)
{
    cookieDatabase().deleteAllCookies();
    completionHandler();
}

} // namespace WebCore

#endif // PLATFORM(HAIKU) && USE(CURL)
