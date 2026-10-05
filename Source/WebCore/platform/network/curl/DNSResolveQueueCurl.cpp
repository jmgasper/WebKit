/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DNSResolveQueueCurl.h"

#if USE(CURL)

#include "NotImplemented.h"
#include <wtf/TZoneMallocInlines.h>

#if PLATFORM(HAIKU)
#include "CurlContext.h"
#include <cstdlib>
#include <wtf/MainThread.h>
#include <wtf/Threading.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DNSResolveQueueCurl);

#if PLATFORM(HAIKU)
void DNSResolveQueueCurl::updateIsUsingProxy()
{
    // Through a proxy, the names a page uses are the proxy's to resolve.
    auto mode = CurlContext::singleton().proxySettings().mode();
    bool fromEnvironment = mode == CurlProxySettings::Mode::Default
        && (getenv("all_proxy") || getenv("ALL_PROXY") || getenv("https_proxy") || getenv("HTTPS_PROXY") || getenv("http_proxy"));
    m_isUsingProxy = mode == CurlProxySettings::Mode::Custom || fromEnvironment;
}

static curl_socket_t refuseSocketHaiku(void*, curlsocktype, struct curl_sockaddr*)
{
    return CURL_SOCKET_BAD;
}

// <link rel=dns-prefetch> and the like: the name is resolved into libcurl's
// DNS cache, which every transfer of the network process shares, so the
// first request to the server does not wait for DNS (30-50 ms here, a round
// trip to the resolver; Haiku keeps no cache of its own). Nothing connects:
// the socket libcurl asks for is refused once the address is known.
void DNSResolveQueueCurl::platformResolve(const String& hostname)
{
    auto& context = CurlContext::singleton();
    auto url = makeString("https://"_s, hostname, '/').utf8();
    Thread::create("DNS prefetch"_s, [url = WTF::move(url), share = context.shareHandle().handle(), timeout = context.dnsCacheTimeout()] {
        if (CURL* handle = curl_easy_init()) {
            curl_easy_setopt(handle, CURLOPT_URL, url.data());
            curl_easy_setopt(handle, CURLOPT_SHARE, share);
            curl_easy_setopt(handle, CURLOPT_DNS_CACHE_TIMEOUT, static_cast<long>(timeout.seconds()));
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 1L);
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, refuseSocketHaiku);
            curl_easy_perform(handle);
            curl_easy_cleanup(handle);
        }
        callOnMainThread([] {
            DNSResolveQueue::singleton().decrementRequestCount();
        });
    }, ThreadType::Network)->detach();
}
#else
void DNSResolveQueueCurl::updateIsUsingProxy()
{
    notImplemented();
}

void DNSResolveQueueCurl::platformResolve(const String& /* hostname */)
{
    notImplemented();
}
#endif

void DNSResolveQueueCurl::resolve(const String& /* hostname */, uint64_t /* identifier */, DNSCompletionHandler&& /* completionHandler */)
{
    notImplemented();
}

void DNSResolveQueueCurl::stopResolve(uint64_t /* identifier */)
{
    notImplemented();
}

}

#endif
