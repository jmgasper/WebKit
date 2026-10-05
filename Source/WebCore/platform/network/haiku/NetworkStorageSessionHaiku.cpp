/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
 * Copyright (C) 2013 University of Szeged. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS''
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

#include <support/Locker.h>
#include <UrlContext.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

#include "Cookie.h"
#include "CookieRequestHeaderFieldProxy.h"
#include "HTTPCookieAcceptPolicy.h"
#include "NetworkingContext.h"
#include "NotImplemented.h"
#include "ResourceHandle.h"
#include "wtf/URL.h"

#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

#define TRACE_COOKIE_JAR 0

namespace WebCore {

CookieStorageSession::CookieStorageSession(PAL::SessionID sessionID)
    : m_sessionID(sessionID)
    , m_context(nullptr)
{
}

CookieStorageSession::~CookieStorageSession()
{
}

static std::unique_ptr<CookieStorageSession>& defaultSession()
{
    ASSERT(isMainThread());
    static NeverDestroyed<std::unique_ptr<CookieStorageSession>> session;
    return session;
}

void CookieStorageSession::setCookiesFromDOM(const URL& firstParty,
        const SameSiteInfo& sameSiteInfo, const URL& url,
        std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID,
        ApplyTrackingPrevention, const String& value, ShouldRelaxThirdPartyCookieBlocking) const
{
    BPrivate::Network::BNetworkCookie* heapCookie
        = new BPrivate::Network::BNetworkCookie(value, BUrl(url));

#if TRACE_COOKIE_JAR
    printf("CookieJar: Add %s for %s\n", heapCookie->RawCookie(true).String(),
        url.string().utf8().legacyCStringPointer());
    printf("  from %s\n", value.utf8().legacyCStringPointer());
#endif
    platformSession().GetCookieJar().AddCookie(heapCookie);
}

HTTPCookieAcceptPolicy CookieStorageSession::cookieAcceptPolicy() const
{
    return HTTPCookieAcceptPolicy::AlwaysAccept;
}

std::pair<String, bool> CookieStorageSession::cookiesForDOM(const URL& firstParty,
        const SameSiteInfo& sameSiteInfo, const URL& url,
        std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID,
        IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention,
        ShouldRelaxThirdPartyCookieBlocking) const
{
#if TRACE_COOKIE_JAR
	printf("CookieJar: Request for %s\n", url.string().utf8().legacyCStringPointer());
#endif

	BString result;
	BUrl hUrl(url);
	bool secure = false;

	const BPrivate::Network::BNetworkCookie* c;
	for (BPrivate::Network::BNetworkCookieJar::UrlIterator it(
            platformSession().GetCookieJar().GetUrlIterator(hUrl));
		    (c = it.Next()); ) {
        // filter out httpOnly cookies,as this method is used to get cookies
        // from JS code and these shouldn't be visible there.
        if(c->HttpOnly())
			continue;

		// filter out secure cookies if they should be
		if (c->Secure())
		{
			secure = true;
            if (includeSecureCookies == IncludeSecureCookies::No)
				continue;
		}
		
		result << "; " << c->RawCookie(false);
	}
	result.Remove(0, 2);

    return {String::fromUTF8(result), secure};
}

void CookieStorageSession::setCookies(const Vector<Cookie>&, const URL&, const URL&)
{
    // FIXME: Implement for WebKit to use.
}

void CookieStorageSession::setCookie(const Cookie&)
{
    // FIXME: Implement for WebKit to use.
}

void CookieStorageSession::deleteCookie(const Cookie&, WTF::CompletionHandler<void()>&&)
{
    // FIXME: Implement for WebKit to use.
}

void CookieStorageSession::deleteCookie(const URL& url, const String& cookie, WTF::CompletionHandler<void()>&&) const
{
#if TRACE_COOKIE_JAR
	printf("CookieJar: delete cookie for %s (NOT IMPLEMENTED)\n", url.string().utf8().legacyCStringPointer());
#endif
	notImplemented();
}

void CookieStorageSession::deleteAllCookies(WTF::CompletionHandler<void()>&&)
{
    notImplemented();
}

void CookieStorageSession::deleteAllCookiesModifiedSince(WallTime since, WTF::CompletionHandler<void()>&&)
{
    notImplemented();
}

void CookieStorageSession::deleteCookiesForHostnames(const Vector<String>& cookieHostNames,
    WebCore::IncludeHttpOnlyCookies, WebCore::ScriptWrittenCookiesOnly, WTF::CompletionHandler<void()>&&)
{
    notImplemented();
}

Vector<Cookie> CookieStorageSession::getAllCookies()
{
    // FIXME: Implement for WebKit to use.
    return { };
}

void CookieStorageSession::getHostnamesWithCookies(HashSet<String>& hostnames)
{
    notImplemented();
}

Vector<Cookie> CookieStorageSession::getCookies(const URL&)
{
    // FIXME: Implement for WebKit to use.
    return { };
}

void CookieStorageSession::hasCookies(const RegistrableDomain&, CompletionHandler<void(bool)>&& completionHandler) const
{
    // FIXME: Implement.
    completionHandler(false);
}

bool CookieStorageSession::getRawCookies(const URL& firstParty,
	const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID,
	std::optional<PageIdentifier> pageID, ApplyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking, Vector<Cookie>& rawCookies) const
{
#if TRACE_COOKIE_JAR
	printf("CookieJar: get raw cookies for %s (NOT IMPLEMENTED)\n", url.string().utf8().legacyCStringPointer());
#endif
	notImplemented();

    rawCookies.clear();
    return false; // return true when implemented
}

std::pair<String, bool> CookieStorageSession::cookieRequestHeaderFieldValue(const URL& firstParty,
	const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID,
	std::optional<PageIdentifier> pageID, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention,
	ShouldRelaxThirdPartyCookieBlocking) const
{
#if TRACE_COOKIE_JAR
	printf("CookieJar: RequestHeaderField for %s\n", url.string().utf8().legacyCStringPointer());
#endif

	BString result;
	BUrl hUrl(url);
	bool secure = false;

	const BPrivate::Network::BNetworkCookie* c;
	for (BPrivate::Network::BNetworkCookieJar::UrlIterator it(
        	platformSession().GetCookieJar().GetUrlIterator(hUrl));
		    (c = it.Next()); ) {
		// filter out secure cookies if they should be
		if (c->Secure())
		{
			secure = true;
            if (includeSecureCookies == IncludeSecureCookies::No)
				continue;
		}
		
		result << "; " << c->RawCookie(false);
	}
	result.Remove(0, 2);

    return {String::fromUTF8(result.String()), secure};
}

std::pair<String, bool> CookieStorageSession::cookieRequestHeaderFieldValue(
    const CookieRequestHeaderFieldProxy& headerFieldProxy) const
{
    return cookieRequestHeaderFieldValue(headerFieldProxy.firstParty,
        headerFieldProxy.sameSiteInfo, headerFieldProxy.url,
        headerFieldProxy.frameID, headerFieldProxy.pageID,
        headerFieldProxy.includeSecureCookies, ApplyTrackingPrevention::Yes,
        ShouldRelaxThirdPartyCookieBlocking::No);
}

BPrivate::Network::BUrlContext& CookieStorageSession::platformSession() const
{
    if (m_context)
        return *m_context;

    // This is the only way to access the default context in the netservces lib
    static BPrivate::Network::BUrlContext* sDefaultContext = nullptr;

    if (sDefaultContext == nullptr) {
        BPrivate::Network::BUrlRequest* fakeRequest
            = BPrivate::Network::BUrlProtocolRoster::MakeRequest(BUrl("data:"), NULL, NULL);
        sDefaultContext = fakeRequest->Context();
        delete fakeRequest;
    }
    return *sDefaultContext;
}

void CookieStorageSession::setPlatformSession(BPrivate::Network::BUrlContext* context)
{
    m_context = context;
}

}

