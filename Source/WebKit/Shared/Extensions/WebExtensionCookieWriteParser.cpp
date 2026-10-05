/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionCookieWriteParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionCookieStoreIdentifier.h"
#include <WebCore/PublicSuffixStore.h>
#include <WebCore/SecurityOrigin.h>
#include <cmath>
#include <limits>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>
namespace WebKit {
static bool hasInvalidCookieText(const String& text)
{
    for (unsigned index = 0; index < text.length(); ++index) {
        auto character = text[index];
        if (character < 0x20 || character == 0x7f || character == ';')
            return true;
    }
    return false;
}

std::expected<WebExtensionCookieWrite, String> parseWebExtensionCookieWrite(const JSON::Value* value)
{
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return std::unexpected("cookie details must be an object"_s);
    WebExtensionCookieWrite result;
    auto& cookie = result.cookie;
    cookie.name = emptyString();
    cookie.value = emptyString();
    cookie.session = true;
    std::optional<String> domain;
    std::optional<String> path;
    bool explicitNoRestriction = false;
    for (auto& key : object->keys()) {
        Ref field = object->getValue(key).releaseNonNull();
        if (key == "secure"_s || key == "httpOnly"_s) {
            auto flag = field->asBoolean();
            if (!flag)
                return std::unexpected("cookie flags must be booleans"_s);
            if (key == "secure"_s)
                cookie.secure = *flag;
            else
                cookie.httpOnly = *flag;
            continue;
        }
        if (key == "expirationDate"_s) {
            auto seconds = field->asDouble();
            auto milliseconds = seconds ? *seconds * 1000.0 : std::numeric_limits<double>::quiet_NaN();
            if (!std::isfinite(milliseconds) || milliseconds < static_cast<double>(std::numeric_limits<int64_t>::min())
                || milliseconds >= static_cast<double>(std::numeric_limits<int64_t>::max()))
                return std::unexpected("cookie expirationDate must be a finite representable number"_s);
            cookie.expires = milliseconds;
            cookie.session = false;
            continue;
        }
        if (key != "url"_s && key != "name"_s && key != "value"_s && key != "domain"_s
            && key != "path"_s && key != "sameSite"_s && key != "storeId"_s)
            return std::unexpected("unsupported cookie write field"_s);
        if (field->type() != JSON::Value::Type::String)
            return std::unexpected("cookie text fields must be strings"_s);
        auto text = field->asString();
        if (text.contains('\0'))
            return std::unexpected("cookie text must not contain null characters"_s);
        if (key == "url"_s) {
            result.url = URL { text };
            if (!result.url.isValid() || !result.url.protocolIsInHTTPFamily() || result.url.host().isEmpty())
                return std::unexpected("cookie URL must be a valid HTTP or HTTPS URL"_s);
        } else if (key == "name"_s)
            cookie.name = text;
        else if (key == "value"_s)
            cookie.value = text;
        else if (key == "domain"_s)
            domain = text;
        else if (key == "path"_s)
            path = text;
        else if (key == "storeId"_s) {
            result.session = parseWebExtensionCookieStoreIdentifier(text);
            if (!result.session)
                return std::unexpected("invalid cookie store identifier"_s);
        } else if (text == "lax"_s)
            cookie.sameSite = WebCore::Cookie::SameSitePolicy::Lax;
        else if (text == "strict"_s)
            cookie.sameSite = WebCore::Cookie::SameSitePolicy::Strict;
        else if (text == "no_restriction"_s)
            explicitNoRestriction = true;
        else
            return std::unexpected("unsupported cookie SameSite value"_s);
    }
    if (!result.url.isValid())
        return std::unexpected("cookie set requires url"_s);
    if (hasInvalidCookieText(cookie.name) || hasInvalidCookieText(cookie.value) || cookie.name.contains('='))
        return std::unexpected("invalid cookie name or value"_s);
    if (cookie.name.utf8().length() + cookie.value.utf8().length() > 4096)
        return std::unexpected("cookie name and value exceed 4096 bytes"_s);

    auto host = result.url.host().toString();
    cookie.domain = host;
    bool hasDomainAttribute = domain && !domain->isEmpty();
    if (hasDomainAttribute) {
        auto text = *domain;
        if (text.startsWith('.'))
            text = text.substring(1);
        if (text.isEmpty() || text.startsWith('.') || hasInvalidCookieText(text) || text.contains('/')
            || text.contains('\\') || text.contains('@') || text.contains('?') || text.contains('#'))
            return std::unexpected("invalid cookie domain"_s);
        if (text.contains(':') && !text.startsWith('['))
            text = makeString('[', text, ']');
        URL domainURL { makeString("https://"_s, text, '/') };
        if (!domainURL.isValid() || domainURL.host().isEmpty() || domainURL.port() || !domainURL.user().isEmpty() || !domainURL.password().isEmpty())
            return std::unexpected("invalid cookie domain"_s);
        auto canonicalDomain = domainURL.host().toString();
        // URL::host() retains IPv6 brackets; hostIsIPAddress expects the address.
        auto address = host.startsWith('[') && host.endsWith(']') ? host.substring(1, host.length() - 2) : host;
        bool isIPAddress = URL::hostIsIPAddress(address);
        if (host != canonicalDomain && (isIPAddress || !host.endsWith(makeString('.', canonicalDomain))))
            return std::unexpected("cookie domain does not match its URL"_s);
        auto suffix = canonicalDomain.endsWith('.') ? canonicalDomain.left(canonicalDomain.length() - 1) : canonicalDomain;
        bool isPublicSuffix = WebCore::PublicSuffixStore::singleton().isPublicSuffix(suffix);
        if (isPublicSuffix && host != canonicalDomain)
            return std::unexpected("cookie domain is a public suffix"_s);
        if (!isIPAddress && !isPublicSuffix)
            cookie.domain = makeString('.', canonicalDomain);
    }
    cookie.path = path && !path->isEmpty() ? *path : WebCore::CookieUtil::defaultPathForURL(result.url);
    if (!cookie.path.startsWith('/') || hasInvalidCookieText(cookie.path))
        return std::unexpected("invalid cookie path"_s);
    if (cookie.secure && !WebCore::SecurityOrigin::create(result.url)->isPotentiallyTrustworthy())
        return std::unexpected("secure cookies require a trustworthy URL"_s);
    if (explicitNoRestriction && !cookie.secure)
        return std::unexpected("SameSite no_restriction requires secure"_s);
    if (cookie.name.startsWith("__Secure-"_s) && !cookie.secure)
        return std::unexpected("__Secure- cookies require secure"_s);
    if (cookie.name.startsWith("__Host-"_s) && (!cookie.secure || hasDomainAttribute || cookie.path != "/"_s))
        return std::unexpected("__Host- cookies require secure, path / and no domain"_s);
    if ((cookie.name.startsWith("__Http-"_s) || cookie.name.startsWith("__Host-Http-"_s)) && (!cookie.secure || !cookie.httpOnly))
        return std::unexpected("HTTP-prefixed cookies require secure and httpOnly"_s);
    cookie.created = WallTime::now().secondsSinceEpoch().milliseconds();
    return result;
}
}
#endif
