/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
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
#include "CurlRequest.h"

#if USE(CURL)

#include "CertificateInfo.h"
#include "CurlRequestClient.h"
#include "CurlRequestScheduler.h"
#include "HTTPParsers.h"
#include "MIMETypeRegistry.h"
#include "NetworkLoadMetrics.h"
#include "ResourceError.h"
#include "SharedBuffer.h"
#include "SynchronousLoaderClient.h"
#include <wtf/CrossThreadCopier.h>
#include "wtf/FileSystem.h"
#include <wtf/Language.h>
#include <wtf/MainThread.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#if PLATFORM(HAIKU)
#include <cstdio>
#include <dlfcn.h>
#include <openssl/ssl.h>
#include <zlib.h>
#include <wtf/HashSet.h>
#include <wtf/text/CString.h>
#endif

namespace WebCore {

#if PLATFORM(HAIKU)
// SUMMIT_NET_TRACE=1 logs every request when it is queued and when it
// completes, fails or is cancelled, to find what holds a page's load event.
static bool netTraceHaiku()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_NET_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}

static double netTraceMsHaiku(MonotonicTime from, MonotonicTime to)
{
    return from && to ? (to - from).milliseconds() : -1;
}

static CString netTraceURLHaiku(const URL& url)
{
    auto string = url.string();
    return (string.length() > 160 ? string.left(160) : string).utf8();
}

// https servers whose connections were seen to be HTTP/1.1, by scheme, host
// and port: connecting to them ahead of a load is wasted.
static Lock http1ServersLockHaiku;

static HashSet<String>& http1ServersHaiku() WTF_REQUIRES_LOCK(http1ServersLockHaiku)
{
    static NeverDestroyed<HashSet<String>> servers;
    return servers;
}

static void noteServerProtocolHaiku(const URL& url, bool http1)
{
    if (!url.protocolIs("https"_s))
        return;
    auto server = url.protocolHostAndPort();
    Locker locker { http1ServersLockHaiku };
    auto& servers = http1ServersHaiku();
    if (!http1)
        servers.remove(server);
    else if (servers.size() < 10000)
        servers.add(WTF::move(server));
}

bool CurlRequest::canPreconnectHaiku(const URL& url)
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_PRECONNECT");
        return !value || strcmp(value, "0");
    }();
    if (!enabled || !url.protocolIs("https"_s))
        return false;
    Locker locker { http1ServersLockHaiku };
    return !http1ServersHaiku().contains(url.protocolHostAndPort());
}

// Content-Encoding. libcurl would decode gzip and deflate itself, but the
// system's libcurl is built without brotli and zstd, which the large sites
// serve to every other browser: YouTube's front page is 98 KB as brotli and
// 255 KB as gzip. Their decoders are system libraries, loaded when first
// needed; without them only gzip and deflate are asked for, as before.
struct BrotliDecoderApiHaiku {
    void* (*create)(void*, void*, void*) { nullptr };
    int (*decompressStream)(void*, size_t*, const uint8_t**, size_t*, uint8_t**, size_t*) { nullptr };
    void (*destroy)(void*) { nullptr };
};

struct ZstdInBufferHaiku {
    const void* src;
    size_t size;
    size_t pos;
};

struct ZstdOutBufferHaiku {
    void* dst;
    size_t size;
    size_t pos;
};

struct ZstdApiHaiku {
    void* (*create)() { nullptr };
    size_t (*free)(void*) { nullptr };
    size_t (*decompressStream)(void*, ZstdOutBufferHaiku*, ZstdInBufferHaiku*) { nullptr };
    unsigned (*isError)(size_t) { nullptr };
};

static const BrotliDecoderApiHaiku* brotliApiHaiku()
{
    static const BrotliDecoderApiHaiku* api = [] () -> BrotliDecoderApiHaiku* {
        void* library = dlopen("libbrotlidec.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return nullptr;
        auto* api = new BrotliDecoderApiHaiku;
        api->create = reinterpret_cast<decltype(api->create)>(dlsym(library, "BrotliDecoderCreateInstance"));
        api->decompressStream = reinterpret_cast<decltype(api->decompressStream)>(dlsym(library, "BrotliDecoderDecompressStream"));
        api->destroy = reinterpret_cast<decltype(api->destroy)>(dlsym(library, "BrotliDecoderDestroyInstance"));
        if (api->create && api->decompressStream && api->destroy)
            return api;
        delete api;
        return nullptr;
    }();
    return api;
}

static const ZstdApiHaiku* zstdApiHaiku()
{
    static const ZstdApiHaiku* api = [] () -> ZstdApiHaiku* {
        void* library = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return nullptr;
        auto* api = new ZstdApiHaiku;
        api->create = reinterpret_cast<decltype(api->create)>(dlsym(library, "ZSTD_createDStream"));
        api->free = reinterpret_cast<decltype(api->free)>(dlsym(library, "ZSTD_freeDStream"));
        api->decompressStream = reinterpret_cast<decltype(api->decompressStream)>(dlsym(library, "ZSTD_decompressStream"));
        api->isError = reinterpret_cast<decltype(api->isError)>(dlsym(library, "ZSTD_isError"));
        if (api->create && api->free && api->decompressStream && api->isError)
            return api;
        delete api;
        return nullptr;
    }();
    return api;
}

// SUMMIT_CONTENT_ENCODINGS=curl leaves decoding to libcurl (gzip, deflate).
static bool decodesContentHaiku()
{
    static const bool decodes = [] {
        const char* value = getenv("SUMMIT_CONTENT_ENCODINGS");
        return !(value && !strcmp(value, "curl"));
    }();
    return decodes;
}

static const String& acceptedEncodingsHaiku()
{
    static NeverDestroyed<String> encodings = [] {
        StringBuilder builder;
        builder.append("gzip, deflate"_s);
        if (brotliApiHaiku())
            builder.append(", br"_s);
        if (zstdApiHaiku())
            builder.append(", zstd"_s);
        return builder.toString();
    }();
    return encodings;
}

class ContentDecoderHaiku {
public:
    enum class Kind : uint8_t { Deflate, Gzip, Brotli, Zstd };

    // One decoder per coding, in the order they are undone (the header lists
    // them in the order they were applied). Null for identity, or a coding
    // that cannot be undone here, which leaves the body as it came, as libcurl
    // would have failed it.
    static std::unique_ptr<ContentDecoderHaiku> create(const String& header)
    {
        Vector<Kind> kinds;
        for (auto token : StringView(header).split(',')) {
            auto coding = token.trim(isASCIIWhitespace<char16_t>);
            if (coding.isEmpty() || equalLettersIgnoringASCIICase(coding, "identity"_s))
                continue;
            if (equalLettersIgnoringASCIICase(coding, "gzip"_s) || equalLettersIgnoringASCIICase(coding, "x-gzip"_s))
                kinds.insert(0, Kind::Gzip);
            else if (equalLettersIgnoringASCIICase(coding, "deflate"_s))
                kinds.insert(0, Kind::Deflate);
            else if (equalLettersIgnoringASCIICase(coding, "br"_s) && brotliApiHaiku())
                kinds.insert(0, Kind::Brotli);
            else if (equalLettersIgnoringASCIICase(coding, "zstd"_s) && zstdApiHaiku())
                kinds.insert(0, Kind::Zstd);
            else
                return nullptr;
        }
        if (kinds.isEmpty())
            return nullptr;
        return std::unique_ptr<ContentDecoderHaiku>(new ContentDecoderHaiku(WTF::move(kinds)));
    }

    ~ContentDecoderHaiku()
    {
        for (auto& stage : m_stages)
            stage.close();
    }

    // Appends what the input decodes to; false when it is not valid.
    bool decode(std::span<const uint8_t> input, Vector<uint8_t>& output)
    {
        Vector<uint8_t> current;
        current.append(input);
        for (auto& stage : m_stages) {
            Vector<uint8_t> next;
            if (!stage.decode(current.span(), next))
                return false;
            current = WTF::move(next);
        }
        output.appendVector(current);
        return true;
    }

private:
    struct Stage {
        Kind kind;
        bool started { false };
        bool finished { false };
        z_stream zlib { };
        void* state { nullptr };

        void close()
        {
            if ((kind == Kind::Gzip || kind == Kind::Deflate) && started)
                inflateEnd(&zlib);
            else if (kind == Kind::Brotli && state)
                brotliApiHaiku()->destroy(state);
            else if (kind == Kind::Zstd && state)
                zstdApiHaiku()->free(state);
        }

        bool decode(std::span<const uint8_t> input, Vector<uint8_t>& output)
        {
            uint8_t buffer[64 * 1024];
            switch (kind) {
            case Kind::Gzip:
            case Kind::Deflate: {
                if (!started) {
                    if (input.empty())
                        return true;
                    // Some servers send "deflate" without the zlib header
                    // (raw deflate); libcurl takes both, and so does this.
                    int bits = 15 + 32;
                    if (kind == Kind::Deflate && ((input[0] & 0x0f) != 8 || (input.size() > 1 && ((input[0] << 8) | input[1]) % 31)))
                        bits = -15;
                    if (inflateInit2(&zlib, bits) != Z_OK)
                        return false;
                    started = true;
                }
                if (finished)
                    return true;
                zlib.next_in = const_cast<Bytef*>(input.data());
                zlib.avail_in = input.size();
                // Until the input is used up and the output no longer fills
                // the buffer (zlib may hold more than one buffer's worth).
                do {
                    zlib.next_out = buffer;
                    zlib.avail_out = sizeof(buffer);
                    int result = inflate(&zlib, Z_NO_FLUSH);
                    output.append(std::span<const uint8_t> { buffer, sizeof(buffer) - zlib.avail_out });
                    if (result == Z_STREAM_END) {
                        // Another gzip member may follow; anything else is ignored.
                        if (kind == Kind::Gzip && zlib.avail_in && inflateReset(&zlib) == Z_OK)
                            continue;
                        finished = true;
                    } else if (result == Z_BUF_ERROR)
                        break;
                    else if (result != Z_OK)
                        return false;
                } while (!finished && (zlib.avail_in || !zlib.avail_out));
                return true;
            }
            case Kind::Brotli: {
                if (!state && !(state = brotliApiHaiku()->create(nullptr, nullptr, nullptr)))
                    return false;
                size_t availableIn = input.size();
                const uint8_t* nextIn = input.data();
                while (!finished) {
                    size_t availableOut = sizeof(buffer);
                    uint8_t* nextOut = buffer;
                    int result = brotliApiHaiku()->decompressStream(state, &availableIn, &nextIn, &availableOut, &nextOut, nullptr);
                    output.append(std::span<const uint8_t> { buffer, sizeof(buffer) - availableOut });
                    if (result == 1) // BROTLI_DECODER_RESULT_SUCCESS
                        finished = true;
                    else if (result == 2) // NEEDS_MORE_INPUT
                        break;
                    else if (result != 3) // NEEDS_MORE_OUTPUT continues; ERROR fails
                        return false;
                }
                return true;
            }
            case Kind::Zstd: {
                if (!state && !(state = zstdApiHaiku()->create()))
                    return false;
                ZstdInBufferHaiku in { input.data(), input.size(), 0 };
                while (in.pos < in.size) {
                    ZstdOutBufferHaiku out { buffer, sizeof(buffer), 0 };
                    size_t before = in.pos;
                    size_t result = zstdApiHaiku()->decompressStream(state, &out, &in);
                    if (zstdApiHaiku()->isError(result))
                        return false;
                    output.append(std::span<const uint8_t> { buffer, out.pos });
                    if (!out.pos && in.pos == before)
                        break;
                }
                // Flush what the decoder still holds for this input.
                for (;;) {
                    ZstdOutBufferHaiku out { buffer, sizeof(buffer), 0 };
                    ZstdInBufferHaiku empty { nullptr, 0, 0 };
                    size_t result = zstdApiHaiku()->decompressStream(state, &out, &empty);
                    if (zstdApiHaiku()->isError(result))
                        return false;
                    output.append(std::span<const uint8_t> { buffer, out.pos });
                    if (out.pos < out.size)
                        break;
                }
                return true;
            }
            }
            return false;
        }
    };

    explicit ContentDecoderHaiku(Vector<Kind>&& kinds)
    {
        for (auto kind : kinds)
            m_stages.append(Stage { kind });
    }

    Vector<Stage> m_stages;
};

#endif

#if PLATFORM(HAIKU)
// libcurl calls this once the connection is up (resolved, connected, TLS
// and ALPN done) or found in its pool, just before sending the request.
int CurlRequest::willSendRequestCallbackHaiku(void* userData, char*, char*, int, int)
{
    auto& request = *static_cast<CurlRequest*>(userData);
    request.m_preconnectedHaiku = true;
    curl_tlssessioninfo* session = nullptr;
    if (curl_easy_getinfo(request.m_curlHandle->handle(), CURLINFO_TLS_SSL_PTR, &session) == CURLE_OK && session
        && session->backend == CURLSSLBACKEND_OPENSSL && session->internals) {
        const unsigned char* protocol = nullptr;
        unsigned length = 0;
        SSL_get0_alpn_selected(static_cast<SSL*>(session->internals), &protocol, &length);
        request.m_preconnectedMultiplexedHaiku = length == 2 && protocol[0] == 'h' && protocol[1] == '2';
    }
    // Stopped before anything is sent, an HTTP/2 connection stays in the
    // pool (a transfer that ends early only closes connections it cannot
    // share).
    return CURL_PREREQFUNC_ABORT;
}
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(CurlRequest);

CurlRequest::~CurlRequest() = default;

CurlRequest::CurlRequest(const ResourceRequest&request, CurlRequestClient* client, CaptureNetworkLoadMetrics captureExtraMetrics)
    : m_client(client)
    , m_request(request.isolatedCopy())
    , m_startState(StartState::WaitingForStart)
    , m_formDataStream(m_request.httpBody())
    , m_captureExtraMetrics(captureExtraMetrics == CaptureNetworkLoadMetrics::Extended)
{
    ASSERT(isMainThread());
}

void CurlRequest::invalidateClient()
{
    ASSERT(isMainThread());

    m_client = nullptr;
}

void CurlRequest::setAuthenticationScheme(ProtectionSpace::AuthenticationScheme scheme)
{
    switch (scheme) {
    case ProtectionSpace::AuthenticationScheme::HTTPBasic:
        m_authType = CURLAUTH_BASIC;
        break;

    case ProtectionSpace::AuthenticationScheme::HTTPDigest:
        m_authType = CURLAUTH_DIGEST;
        break;

    case ProtectionSpace::AuthenticationScheme::NTLM:
        m_authType = CURLAUTH_NTLM;
        break;

    case ProtectionSpace::AuthenticationScheme::Negotiate:
        m_authType = CURLAUTH_NEGOTIATE;
        break;

    default:
        m_authType = CURLAUTH_ANY;
        break;
    }
}

void CurlRequest::setUserPass(const String& user, const String& password)
{
    ASSERT(isMainThread());

    m_user = user.isolatedCopy();
    m_password = password.isolatedCopy();
}

void CurlRequest::start()
{
    // The pausing of transfer does not work with protocols, like file://.
    // Therefore, PAUSE can not be done in didReceiveData().
    // It means that the same logic as http:// can not be used.
    // In the file scheme, invokeDidReceiveResponse() is done first. 
    // Then StartWithJobManager is called with completeDidReceiveResponse and start transfer with libcurl.

    // http : didReceiveHeader => didReceiveData[PAUSE] => invokeDidReceiveResponse => (MainThread)curlDidReceiveResponse => completeDidReceiveResponse[RESUME] => didReceiveData
    // file : invokeDidReceiveResponseForFile => (MainThread)curlDidReceiveResponse => completeDidReceiveResponse => didReceiveData

    ASSERT(isMainThread());

    switch (m_startState) {
    case StartState::DidStart:
        ASSERT(false);
        [[fallthrough]];
    case StartState::StartSuspended:
        return;
    case StartState::WaitingForStart:
        m_startState = StartState::DidStart;
        break;
    }

    if (m_request.url().protocolIsFile())
        invokeDidReceiveResponseForFile(m_request.url());
    else
        startWithJobManager();
}

void CurlRequest::startWithJobManager()
{
    ASSERT(isMainThread());
#if PLATFORM(HAIKU)
    if (netTraceHaiku()) {
        m_netTraceQueuedAtHaiku = MonotonicTime::now();
        fprintf(stderr, "Summit net: start id=%p %s %s\n", this, m_request.httpMethod().utf8().data(), netTraceURLHaiku(m_request.url()).data());
    }
#endif

    CurlContext::singleton().scheduler().add(this, m_request.url().host().toString());
}

void CurlRequest::cancel()
{
    ASSERT(isMainThread());

    {
        Locker locker { m_statusMutex };
        if (m_cancelled)
            return;

        m_cancelled = true;
    }
#if PLATFORM(HAIKU)
    if (netTraceHaiku() && m_netTraceQueuedAtHaiku)
        fprintf(stderr, "Summit net: cancel id=%p after=%.0f %s\n", this, netTraceMsHaiku(m_netTraceQueuedAtHaiku, MonotonicTime::now()), netTraceURLHaiku(m_request.url()).data());
#endif

    auto& scheduler = CurlContext::singleton().scheduler();

    if (needToInvokeDidCancelTransfer()) {
        runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }]() {
            didCancelTransfer();
        });
    } else if (m_startState == StartState::DidStart)
        scheduler.cancel(this);

    invalidateClient();
}

bool CurlRequest::isCancelled()
{
    Locker locker { m_statusMutex };
    return m_cancelled;
}

bool CurlRequest::isCompletedOrCancelled()
{
    Locker locker { m_statusMutex };
    return m_completed || m_cancelled;
}

void CurlRequest::suspend()
{
    ASSERT(isMainThread());

    switch (m_startState) {
    case StartState::StartSuspended:
        ASSERT(false);
        [[fallthrough]];
    case StartState::WaitingForStart:
        m_startState = StartState::StartSuspended;
        break;
    case StartState::DidStart:
        setRequestPaused(true);
        break;
    }
}

void CurlRequest::resume()
{
    ASSERT(isMainThread());

    switch (m_startState) {
    case StartState::WaitingForStart:
        ASSERT(false);
        [[fallthrough]];
    case StartState::StartSuspended:
        m_startState = StartState::WaitingForStart;
        start();
        break;
    case StartState::DidStart:
        setRequestPaused(false);
        break;
    }
}

/* `this` is protected inside this method. */
void CurlRequest::callClient(Function<void(CurlRequest&, CurlRequestClient&)>&& task)
{
    runOnMainThread([this, protectedThis = Ref { *this }, task = WTF::move(task)]() mutable {
        if (m_client)
            task(*this, Ref { *m_client });
    });
}

void CurlRequest::runOnMainThread(Function<void()>&& task)
{
    ensureOnMainThread(std::move(task));
}

void CurlRequest::runOnWorkerThreadIfRequired(Function<void()>&& task)
{
    if (isMainThread())
        CurlContext::singleton().scheduler().callOnWorkerThread(this, WTF::move(task));
    else
        task();
}

CURL* CurlRequest::setupTransfer()
{
    auto httpHeaderFields = m_request.httpHeaderFields();
    appendAcceptLanguageHeaderIfNeeded(httpHeaderFields);

    m_curlHandle = makeUnique<CurlHandle>();
#if PLATFORM(HAIKU)
    m_decodesContentHaiku = decodesContentHaiku() && m_request.url().protocolIsInHTTPFamily();
    if (m_decodesContentHaiku) {
        curl_easy_setopt(m_curlHandle->handle(), CURLOPT_ACCEPT_ENCODING, static_cast<const char*>(nullptr));
        curl_easy_setopt(m_curlHandle->handle(), CURLOPT_HTTP_CONTENT_DECODING, 0L);
        // A part of a resource is asked for as it is stored: a coding
        // applies to the whole, and a part of it could not be decoded.
        if (!httpHeaderFields.contains(HTTPHeaderName::AcceptEncoding))
            httpHeaderFields.set(HTTPHeaderName::AcceptEncoding, httpHeaderFields.contains(HTTPHeaderName::Range) ? "identity"_s : acceptedEncodingsHaiku());
    }
#endif

    m_curlHandle->setURL(m_request.url(), m_localhostAlias);

    m_curlHandle->appendRequestHeaders(httpHeaderFields);

    const auto& method = m_request.httpMethod();
    if (method == "GET"_s)
        m_curlHandle->enableHttpGetRequest();
    else if (method == "POST"_s)
        setupPOST();
    else if (method == "PUT"_s)
        setupPUT();
    else if (method == "HEAD"_s)
        m_curlHandle->enableHttpHeadRequest();
    else {
        m_curlHandle->setHttpCustomRequest(method);
        setupPUT();
    }

    if (!m_user.isEmpty() || !m_password.isEmpty()) {
        m_curlHandle->setHttpAuthUserPass(m_user, m_password, m_authType);
    }

    if (m_shouldDisableServerTrustEvaluation)
        m_curlHandle->disableServerTrustEvaluation();

    m_curlHandle->setHeaderCallbackFunction(didReceiveHeaderCallback, this);
    m_curlHandle->setWriteCallbackFunction(didReceiveDataCallback, this);
    m_curlHandle->setDebugCallbackFunction(didReceiveDebugInfoCallback, this);
#if PLATFORM(HAIKU)
    if (m_preconnectOnlyHaiku) {
        curl_easy_setopt(m_curlHandle->handle(), CURLOPT_PREREQFUNCTION, willSendRequestCallbackHaiku);
        curl_easy_setopt(m_curlHandle->handle(), CURLOPT_PREREQDATA, this);
    }
#endif

    m_curlHandle->setTimeout(timeoutInterval());

    m_performStartTime = MonotonicTime::now();

    return m_curlHandle->handle();
}

Seconds CurlRequest::timeoutInterval() const
{
    // Request specific timeout interval.
    if (m_request.timeoutInterval())
        return Seconds { m_request.timeoutInterval() };

    // Default timeout interval set by application.
    if (m_request.defaultTimeoutInterval())
        return Seconds { m_request.defaultTimeoutInterval() };

    // Use platform default timeout interval.
    return CurlContext::singleton().defaultTimeoutInterval();
}

// This is called to obtain HTTP POST or PUT data.
// Iterate through FormData elements and upload files.
// Carefully respect the given buffer size and fill the rest of the data at the next calls.

size_t CurlRequest::willSendData(char* buffer, size_t blockSize, size_t numberOfBlocks)
{
    if (isCompletedOrCancelled())
        return CURL_READFUNC_ABORT;

    if (!blockSize || !numberOfBlocks)
        return CURL_READFUNC_ABORT;

    // Check for overflow.
    if (blockSize > (std::numeric_limits<size_t>::max() / numberOfBlocks))
        return CURL_READFUNC_ABORT;

    size_t bufferSize = blockSize * numberOfBlocks;
    auto sendBytes = m_formDataStream.read(buffer, bufferSize);
    if (!sendBytes) {
        // Something went wrong so error the job.
        return CURL_READFUNC_ABORT;
    }

    callClient([totalReadSize = m_formDataStream.totalReadSize(), totalSize = m_formDataStream.totalSize()](CurlRequest& request, CurlRequestClient& client) {
        client.curlDidSendData(request, totalReadSize, totalSize);
    });

    return *sendBytes;
}

// The request timeout is an idle timeout (CurlHandle::setTimeout), so it also
// ended responses that stream for as long as the page shows them and may go
// quiet in between: an MJPEG feed from a camera, or from a KVM whose machine
// shows no picture, and an event stream between events. Other browsers keep
// those open; once one has started, only the server or the page ends it.
static bool isStreamingResponse(const CurlResponse& response)
{
    static constexpr auto contentTypeLength = std::char_traits<char>::length("content-type:");

    for (auto& header : response.headers) {
        if (!header.startsWithIgnoringASCIICase("content-type:"_s))
            continue;

        auto mimeType = extractMIMETypeFromMediaType(header.substring(contentTypeLength).trim(deprecatedIsSpaceOrNewline));
        return equalLettersIgnoringASCIICase(mimeType, "multipart/x-mixed-replace"_s)
            || equalLettersIgnoringASCIICase(mimeType, "text/event-stream"_s);
    }

    return false;
}

// This is being called for each HTTP header in the response. This includes '\r\n'
// for the last line of the header.

size_t CurlRequest::didReceiveHeader(String&& header)
{
    static constexpr auto emptyLineCRLF = "\r\n"_s;
    static constexpr auto emptyLineLF = "\n"_s;

    if (isCompletedOrCancelled())
#if LIBCURL_VERSION_NUM >= 0x075700
        return CURL_WRITEFUNC_ERROR;
#else
        return 0;
#endif

    // libcurl sends all headers that libcurl received to application.
    // So, in digest authentication, a block of response headers are received twice consecutively from libcurl.
    // For example, when authentication succeeds, the first block is "401 Authorization", and the second block is "200 OK".
    // Also, "100 Continue" and "200 Connection Established" do the same behavior.
    // In this process, deletes the first block to send a correct headers to WebCore.
    if (m_didReceiveResponse) {
        m_didReceiveResponse = false;
        m_response = CurlResponse { };
        m_multipartHandle = nullptr;
#if PLATFORM(HAIKU)
        m_contentDecoderHaiku = nullptr;
#endif
    }

    auto receiveBytes = static_cast<size_t>(header.length());

    // The HTTP standard requires to use \r\n but for compatibility it recommends to accept also \n.
    if ((header != emptyLineCRLF) && (header != emptyLineLF)) {
        m_response.headers.append(WTF::move(header));
        return receiveBytes;
    }

    long statusCode = 0;
    if (auto code = m_curlHandle->getResponseCode())
        statusCode = *code;

    long httpConnectCode = 0;
    if (auto code = m_curlHandle->getHttpConnectCode())
        httpConnectCode = *code;

    m_didReceiveResponse = true;

    m_response.url = m_request.url();
    m_response.statusCode = statusCode;
    m_response.httpConnectCode = httpConnectCode;

    if (auto length = getContentLength())
        m_response.expectedContentLength = *length;

    if (auto proxyURL = m_curlHandle->getProxyUrl())
        m_response.proxyUrl = URL { *proxyURL };

    if (auto auth = m_curlHandle->getHttpAuthAvail())
        m_response.availableHttpAuth = *auth;

    if (auto auth = m_curlHandle->getProxyAuthAvail())
        m_response.availableProxyAuth = *auth;

    if (auto version = m_curlHandle->getHttpVersion())
        m_response.httpVersion = *version;

    if (m_response.availableProxyAuth)
        CurlContext::singleton().setProxyAuthMethod(m_response.availableProxyAuth);

    if (auto info = m_curlHandle->certificateInfo())
        m_response.certificateInfo = WTF::move(*info);

    m_response.networkLoadMetrics = networkLoadMetrics();

    m_multipartHandle = CurlMultipartHandle::createIfNeeded(*this, m_response);

#if PLATFORM(HAIKU)
    if (m_decodesContentHaiku) {
        for (auto& line : m_response.headers) {
            auto colon = line.find(':');
            if (colon == notFound || !equalLettersIgnoringASCIICase(StringView(line).left(colon).trim(isASCIIWhitespace<char16_t>), "content-encoding"_s))
                continue;
            m_contentDecoderHaiku = ContentDecoderHaiku::create(StringView(line).substring(colon + 1).trim(isASCIIWhitespace<char16_t>).toString());
        }
    }
#endif

    if (isStreamingResponse(m_response))
        m_curlHandle->setTimeout(0_s);

    // Response will send at didReceiveData() or didCompleteTransfer()
    // to receive continueDidRceiveResponse() for asynchronously.

    return receiveBytes;
}

// called with data after all headers have been processed via headerCallback

size_t CurlRequest::didReceiveData(std::span<const uint8_t> receivedData)
{
    if (isCompletedOrCancelled())
#if LIBCURL_VERSION_NUM >= 0x075700
        return CURL_WRITEFUNC_ERROR;
#else
        return 0;
#endif

    if (needToInvokeDidReceiveResponse()) {
        // Pause until completeDidReceiveResponse() is called.
        invokeDidReceiveResponse(m_response, [this] {
            runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }]() {
                if (isCompletedOrCancelled() || !m_curlHandle)
                    return;

                // Unpause a connection
                m_curlHandle->pause(CURLPAUSE_CONT);
            });
        });

        return CURL_WRITEFUNC_PAUSE;
    }

    // What libcurl gave is taken whole, whatever it decodes to.
    size_t consumed = receivedData.size();
#if PLATFORM(HAIKU)
    Vector<uint8_t> decoded;
    if (m_contentDecoderHaiku) {
        if (!m_contentDecoderHaiku->decode(receivedData, decoded)) {
            if (netTraceHaiku())
                fprintf(stderr, "Summit net: content decoding failed id=%p %s\n", this, netTraceURLHaiku(m_request.url()).data());
            return CURL_WRITEFUNC_ERROR;
        }
        receivedData = decoded.span();
    }
#endif

    m_totalReceivedSize += receivedData.size();

    writeDataToDownloadFileIfEnabled(receivedData);

    if (receivedData.size()) {
        if (m_multipartHandle) {
            m_multipartHandle->didReceiveMessage(receivedData);
            if (m_multipartHandle->hasError())
#if LIBCURL_VERSION_NUM >= 0x075700
                return CURL_WRITEFUNC_ERROR;
#else
                return 0;
#endif
        } else {
            callClient([buffer = SharedBuffer::create(receivedData)](CurlRequest& request, CurlRequestClient& client) mutable {
                client.curlDidReceiveData(request, WTF::move(buffer));
            });
        }
    }

    return consumed;
}

void CurlRequest::didReceiveHeaderFromMultipart(Vector<String>&& headers)
{
    if (isCompletedOrCancelled())
        return;

    CurlResponse response = m_response.isolatedCopy();
    response.expectedContentLength = 0;
    response.headers.clear();

    for (auto& header : headers)
        response.headers.append(WTF::move(header));

    invokeDidReceiveResponse(response, [this] {
        runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }]() {
            if (isCompletedOrCancelled() || !m_multipartHandle)
                return;

            m_multipartHandle->completeHeaderProcessing();
        });
    });
}

void CurlRequest::didReceiveDataFromMultipart(std::span<const uint8_t> receivedData)
{
    if (isCompletedOrCancelled())
        return;

    if (receivedData.size()) {
        callClient([buffer = SharedBuffer::create(receivedData)](CurlRequest& request, CurlRequestClient& client) mutable {
            client.curlDidReceiveData(request, WTF::move(buffer));
        });
    }
}

void CurlRequest::didCompleteFromMultipart()
{
    ASSERT(m_multipartHandle && m_multipartHandle->completed());

    runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }]() {
        didCompleteTransfer(CURLE_OK);
    });
}

void CurlRequest::didCompleteTransfer(CURLcode result)
{
    if (isCancelled()) {
        didCancelTransfer();
        return;
    }

#if PLATFORM(HAIKU)
    if (m_preconnectOnlyHaiku && result == CURLE_ABORTED_BY_CALLBACK && m_preconnectedHaiku) {
        // Connected, and stopped there on purpose.
        auto metrics = networkLoadMetrics();
        noteServerProtocolHaiku(m_request.url(), !m_preconnectedMultiplexedHaiku);
        if (netTraceHaiku() && m_netTraceQueuedAtHaiku) {
            fprintf(stderr, "Summit net: preconnected id=%p queue=%.0f dns=%.0f connect=%.0f tls=%.0f total=%.0f proto=%s %s\n",
                this, netTraceMsHaiku(m_netTraceQueuedAtHaiku, m_performStartTime),
                netTraceMsHaiku(metrics.domainLookupStart, metrics.domainLookupEnd), netTraceMsHaiku(metrics.connectStart, metrics.connectEnd),
                netTraceMsHaiku(metrics.secureConnectionStart, metrics.connectEnd), netTraceMsHaiku(m_netTraceQueuedAtHaiku, MonotonicTime::now()),
                m_preconnectedMultiplexedHaiku ? "h2" : "http/1.1 (closed)", netTraceURLHaiku(m_request.url()).data());
        }
        finalizeTransfer();
        callClient([networkLoadMetrics = WTF::move(metrics)](CurlRequest& request, CurlRequestClient& client) mutable {
            networkLoadMetrics.markComplete();
            client.curlDidComplete(request, WTF::move(networkLoadMetrics));
        });
        Locker locker { m_statusMutex };
        m_completed = true;
        return;
    }
#endif

    // Newer versions of curl/OpenSSL report CURLE_RECV_ERROR or CURLE_PARTIAL_FILE
    // when a server closes the connection to signal end-of-body on responses with
    // no Content-Length and no chunked encoding. This is valid HTTP/1.1 behavior,
    // so treat it as success when the response was otherwise complete.
    // See https://github.com/curl/curl/issues/13036
    bool isConnectionCloseEndOfBody = (result == CURLE_RECV_ERROR || result == CURLE_PARTIAL_FILE)
        && m_response.statusCode >= 200 && m_response.statusCode < 400
        && !m_response.expectedContentLength;

    bool isProxyAuthenticationRequired = result == CURLE_RECV_ERROR && m_response.httpConnectCode == 407;
    if (needToInvokeDidReceiveResponse() && (result == CURLE_OK || isProxyAuthenticationRequired || isConnectionCloseEndOfBody)) {
        // Processing of didReceiveResponse() has not been completed. (For example, HEAD Method, Proxy authentication, etc.)
        m_mustInvokeCancelTransfer = true;
        invokeDidReceiveResponse(m_response, [this, result]() mutable {
            m_mustInvokeCancelTransfer = false;
            runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }, result]() {
                didCompleteTransfer(result);
            });
        });
        return;
    }

    if (result == CURLE_OK || isConnectionCloseEndOfBody) {
        if (m_multipartHandle && !m_multipartHandle->completed()) {
            m_multipartHandle->didCompleteMessage();
            return;
        }

        auto metrics = networkLoadMetrics();
#if PLATFORM(HAIKU)
        if (!metrics.protocol.isEmpty())
            noteServerProtocolHaiku(m_request.url(), metrics.protocol == "http/1.1"_s || metrics.protocol == "http/1.0"_s);
        if (netTraceHaiku() && m_netTraceQueuedAtHaiku) {
            auto now = MonotonicTime::now();
            fprintf(stderr, "Summit net: done id=%p status=%d queue=%.0f dns=%.0f connect=%.0f tls=%.0f ttfb=%.0f total=%.0f bytes=%zu proto=%s %s\n",
                this, m_response.statusCode, netTraceMsHaiku(m_netTraceQueuedAtHaiku, m_performStartTime),
                netTraceMsHaiku(metrics.domainLookupStart, metrics.domainLookupEnd), netTraceMsHaiku(metrics.connectStart, metrics.connectEnd),
                netTraceMsHaiku(metrics.secureConnectionStart, metrics.connectEnd), netTraceMsHaiku(metrics.requestStart, metrics.responseStart),
                netTraceMsHaiku(m_netTraceQueuedAtHaiku, now), m_totalReceivedSize, metrics.protocol.utf8().data(),
                netTraceURLHaiku(m_request.url()).data());
        }
#endif

        finalizeTransfer();
        callClient([networkLoadMetrics = WTF::move(metrics)](CurlRequest& request, CurlRequestClient& client) mutable {
            networkLoadMetrics.responseEnd = MonotonicTime::now();
            networkLoadMetrics.markComplete();

            client.curlDidComplete(request, WTF::move(networkLoadMetrics));
        });
    } else {
        auto type = (result == CURLE_OPERATION_TIMEDOUT && timeoutInterval()) ? ResourceError::Type::Timeout : ResourceError::Type::General;
        auto resourceError = ResourceError(result, m_request.url(), type);
#if PLATFORM(HAIKU)
        if (netTraceHaiku() && m_netTraceQueuedAtHaiku)
            fprintf(stderr, "Summit net: fail id=%p result=%d (%s) total=%.0f %s\n", this, static_cast<int>(result), curl_easy_strerror(result),
                netTraceMsHaiku(m_netTraceQueuedAtHaiku, MonotonicTime::now()), netTraceURLHaiku(m_request.url()).data());
#endif

        CertificateInfo certificateInfo;
        if (auto info = m_curlHandle->certificateInfo())
            certificateInfo = WTF::move(*info);

        finalizeTransfer();
        callClient([error = WTF::move(resourceError), certificateInfo = WTF::move(certificateInfo)](CurlRequest& request, CurlRequestClient& client) mutable {
            client.curlDidFailWithError(request, WTF::move(error), WTF::move(certificateInfo));
        });
    }

    {
        Locker locker { m_statusMutex };
        m_completed = true;
    }
}

void CurlRequest::didCancelTransfer()
{
    finalizeTransfer();
    cleanupDownloadFile();
}

void CurlRequest::finalizeTransfer()
{
    closeDownloadFile();
    m_formDataStream.clean();
    m_multipartHandle = nullptr;
    m_curlHandle = nullptr;
}

int CurlRequest::didReceiveDebugInfo(curl_infotype type, std::span<const char> data)
{
    if (!data.data())
        return 0;

    if (type == CURLINFO_HEADER_OUT) {
        String requestHeader = String::fromLatin1(data);
        auto headerFields = requestHeader.split("\r\n"_s);
        // Remove the request line
        if (headerFields.size())
            headerFields.removeAt(0);

        m_requestHeaderSize = requestHeader.length();

        for (auto& header : headerFields) {
            auto pos = header.find(':');
            if (pos != notFound) {
                auto key = header.left(pos).trim(deprecatedIsSpaceOrNewline);
                auto value = header.substring(pos + 1).trim(deprecatedIsSpaceOrNewline);
                m_requestHeaders.add(key, value);
            }
        }
    }

    return 0;
}

void CurlRequest::appendAcceptLanguageHeaderIfNeeded(HTTPHeaderMap& header)
{
    if (header.contains(HTTPHeaderName::AcceptLanguage))
        return;

    for (const auto& language : userPreferredLanguages())
        header.add(HTTPHeaderName::AcceptLanguage, language);
}

void CurlRequest::setupPUT()
{
    curl_off_t totalSize = m_formDataStream.totalSize();

    m_curlHandle->enableHttpPutRequest(totalSize);
    m_curlHandle->setReadCallbackFunction(willSendDataCallback, this);

    // Disable a "Expect: 100-continue" header
    m_curlHandle->removeRequestHeader("Expect"_s);
}

void CurlRequest::setupPOST()
{
    curl_off_t totalSize = m_formDataStream.totalSize();

    m_curlHandle->enableHttpPostRequest(totalSize);
    m_curlHandle->setReadCallbackFunction(willSendDataCallback, this);

    // Override the default POST Content-Type: header
    if (!m_request.hasHTTPHeader(HTTPHeaderName::ContentType) && !totalSize)
        m_curlHandle->removeRequestHeader("Content-Type"_s);

    // Disable a "Expect: 100-continue" header
    m_curlHandle->removeRequestHeader("Expect"_s);
}

void CurlRequest::invokeDidReceiveResponseForFile(const URL& url)
{
    // Since the code in didReceiveHeader() will not have run for local files
    // the code to set the URL and fire didReceiveResponse is never run,
    // which means the ResourceLoader's response does not contain the URL.
    // Run the code here for local files to resolve the issue.

    ASSERT(isMainThread());
    ASSERT(url.protocolIsFile());

    // Determine the MIME type based on the path.
    auto mimeType = MIMETypeRegistry::mimeTypeForPath(url.path().toString());

    // DidReceiveResponse must not be called immediately
    runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }, url = crossThreadCopy(url), mimeType = crossThreadCopy(WTF::move(mimeType))]() mutable {
        CurlResponse response;
        response.url = WTF::move(url);
        response.statusCode = 200;
        response.headers.append(makeString("Content-Type: "_s, mimeType));

        invokeDidReceiveResponse(response, [this] {
            startWithJobManager();
        });
    });
}

void CurlRequest::invokeDidReceiveResponse(const CurlResponse& response, Function<void()>&& completionHandler)
{
    ASSERT(!m_responseCompletionHandler);
    ASSERT(!m_didNotifyResponse || m_multipartHandle);

    m_didNotifyResponse = true;
    m_responseCompletionHandler = WTF::move(completionHandler);

    // FIXME: Replace this isolatedCopy with WTF::move.
    callClient([response = response.isolatedCopy()](CurlRequest& request, CurlRequestClient& client) mutable {
        client.curlDidReceiveResponse(request, WTF::move(response));
    });
}

void CurlRequest::completeDidReceiveResponse()
{
    ASSERT(isMainThread());
    ASSERT(m_didNotifyResponse);
    ASSERT(!m_didReturnFromNotify || m_multipartHandle);

    if (isCompletedOrCancelled())
        return;

    m_didReturnFromNotify = true;

    if (auto responseCompletionHandler = WTF::move(m_responseCompletionHandler))
        responseCompletionHandler();
}

void CurlRequest::setRequestPaused(bool paused)
{
    {
        Locker locker { m_pauseStateMutex };

        auto savedState = shouldBePaused();
        m_isPausedOfRequest = paused;
        if (shouldBePaused() == savedState)
            return;
    }

    pausedStatusChanged();
}

void CurlRequest::setCallbackPaused(bool paused)
{
    {
        Locker locker { m_pauseStateMutex };

        auto savedState = shouldBePaused();
        m_isPausedOfCallback = paused;

        // If pause is requested, it is called within didReceiveData() which means
        // actual change happens inside libcurl. No need to update manually here.
        if (shouldBePaused() == savedState || paused)
            return;
    }

    pausedStatusChanged();
}

void CurlRequest::invokeCancel()
{
    // There's no need to extract this method. This is a workaround for MSVC's bug
    // which happens when using lambda inside other lambda. The compiler loses context
    // of `this` which prevent makeRef.
    runOnMainThread([this, protectedThis = Ref { *this }]() {
        cancel();
    });
}

void CurlRequest::pausedStatusChanged()
{
    if (isCompletedOrCancelled())
        return;

    runOnWorkerThreadIfRequired([this, protectedThis = Ref { *this }]() {
        if (isCompletedOrCancelled() || !m_curlHandle)
            return;

        bool needCancel { false };
        {
            Locker locker { m_pauseStateMutex };
            bool paused = shouldBePaused();

            if (isHandlePaused() == paused)
                return;

            auto error = m_curlHandle->pause(paused ? CURLPAUSE_ALL : CURLPAUSE_CONT);
            if (error == CURLE_OK)
                updateHandlePauseState(paused);

            needCancel = (error != CURLE_OK && !paused);
        }

        if (needCancel)
            invokeCancel();
    });
}

void CurlRequest::updateHandlePauseState(bool paused)
{
    ASSERT(!isMainThread());
    m_isHandlePaused = paused;
}

bool CurlRequest::isHandlePaused() const
{
    ASSERT(!isMainThread());
    return m_isHandlePaused;
}

NetworkLoadMetrics CurlRequest::networkLoadMetrics()
{
    ASSERT(m_curlHandle);

    auto networkLoadMetrics = m_curlHandle->getNetworkLoadMetrics(m_performStartTime);
    if (!networkLoadMetrics)
        return NetworkLoadMetrics();

    networkLoadMetrics->responseBodyDecodedSize = m_totalReceivedSize;

    if (m_captureExtraMetrics) {
        m_curlHandle->addExtraNetworkLoadMetrics(*networkLoadMetrics);
        if (auto* additionalMetrics = networkLoadMetrics->additionalNetworkLoadMetricsForWebInspector.get()) {
            additionalMetrics->requestHeaderBytesSent = m_requestHeaderSize;
            additionalMetrics->requestHeaders = m_requestHeaders;
        }
    }

    return WTF::move(*networkLoadMetrics);
}

void CurlRequest::enableDownloadToFile()
{
    Locker locker { m_downloadMutex };
    m_isEnabledDownloadToFile = true;
}

const String& CurlRequest::getDownloadedFilePath()
{
    Locker locker { m_downloadMutex };
    return m_downloadFilePath;
}

void CurlRequest::writeDataToDownloadFileIfEnabled(std::span<const unsigned char> buffer)
{
    {
        Locker locker { m_downloadMutex };

        if (!m_isEnabledDownloadToFile)
            return;

        if (m_downloadFilePath.isEmpty()) {
            auto [m_downloadFilePath, m_downloadFileHandle] = FileSystem::openTemporaryFile("download"_s);
        } else if (!m_downloadFileHandle) {
            m_downloadFileHandle = FileSystem::openFile(m_downloadFilePath, FileSystem::FileOpenMode::Truncate);
        }
    }

    if (m_downloadFileHandle)
        m_downloadFileHandle.write(buffer);
}

void CurlRequest::closeDownloadFile()
{
    Locker locker { m_downloadMutex };

    if (!m_downloadFileHandle)
        return;

    m_downloadFileHandle.flush();
}

void CurlRequest::cleanupDownloadFile()
{
    Locker locker { m_downloadMutex };

    if (!m_downloadFilePath.isEmpty()) {
        FileSystem::deleteFile(m_downloadFilePath);
        m_downloadFilePath = String();
    }
}

std::optional<long long> CurlRequest::getContentLength()
{
    for (const auto& header : m_response.headers) {
        if (header.startsWithIgnoringASCIICase("content-length:"_s)) {
            if (auto splitPosition = header.find(':'); splitPosition != notFound) {
                auto value = header.substring(splitPosition + 1).trim(deprecatedIsSpaceOrNewline);
                if (auto length = parseContentLength(value))
                    return *length;
            }
        }
    }

    return std::nullopt;
}

size_t CurlRequest::willSendDataCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->willSendData(ptr, blockSize, numberOfBlocks);
}

size_t CurlRequest::didReceiveHeaderCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveHeader(String::fromLatin1(unsafeMakeSpan(ptr, blockSize * numberOfBlocks)));
}

size_t CurlRequest::didReceiveDataCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveData({ reinterpret_cast<const uint8_t*>(ptr), blockSize * numberOfBlocks });
}

int CurlRequest::didReceiveDebugInfoCallback(CURL*, curl_infotype type, char* data, size_t size, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveDebugInfo(type, { data, size });
}

}

#endif
