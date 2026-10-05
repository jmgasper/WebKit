/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <DataIO.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <strings.h>
#include <thread>
#include <utility>
#include <vector>

namespace WebCore {

// Media Kit reads a file through BPositionIO. Downloading the whole file
// first made a video wait for its last byte before its first frame. This
// fetches it with range requests into memory, from wherever the demuxer reads:
// the start, the index at the end of an MP4 that keeps it there, the point a
// seek jumps to. A read blocks only until its own bytes have arrived.
// SUMMIT_MEDIA_STREAMING=0 downloads whole files again.
class StreamingMediaIO final : public BPositionIO {
public:
    static constexpr off_t chunkSize = 256 * 1024;
    static constexpr off_t maximumSize = 512 * 1024 * 1024;
    // Beyond this distance past the download, a read starts a new request
    // instead of waiting for the current one to get there.
    static constexpr off_t reachAhead = 2 * 1024 * 1024;

    // Null when the server does not say how large the file is.
    // Requests carry the page's user agent and referrer: some media hosts
    // refuse anything else.
    static std::unique_ptr<StreamingMediaIO> open(const char* url, std::atomic<bool>& abort,
        const std::string& userAgent = { }, const std::string& referrer = { })
    {
        std::unique_ptr<StreamingMediaIO> io(new StreamingMediaIO(url, abort));
        io->m_userAgent = userAgent;
        io->m_referrer = referrer;
        io->m_thread = std::thread([raw = io.get()] { raw->run(); });
        std::unique_lock lock(io->m_lock);
        while (!io->m_sizeKnown && !io->m_failed && !abort)
            io->m_changed.wait_for(lock, std::chrono::milliseconds(50));
        if (!io->m_sizeKnown || abort)
            return nullptr;
        return io;
    }

    ~StreamingMediaIO()
    {
        {
            std::lock_guard lock(m_lock);
            m_stopping = true;
            if (m_multi)
                curl_multi_wakeup(m_multi);
        }
        m_changed.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    ssize_t ReadAt(off_t position, void* buffer, size_t size) override
    {
        if (position < 0)
            return B_BAD_VALUE;
        if (position >= m_size || !size)
            return 0;
        size = std::min<off_t>(size, m_size - position);
        size_t first = position / chunkSize;
        size_t last = (position + size - 1) / chunkSize;
        std::unique_lock lock(m_lock);
        while (true) {
            size_t missing = first;
            while (missing <= last && complete(missing))
                ++missing;
            if (missing > last)
                break;
            if (m_stopping || m_abort)
                return B_INTERRUPTED;
            if (m_failed)
                return B_IO_ERROR;
            off_t wanted = off_t(missing) * chunkSize;
            // The running request gets there soon enough; otherwise ask for it.
            if (m_acceptsRanges && (wanted < m_streamStart || wanted > m_head + reachAhead) && m_wanted != wanted) {
                m_wanted = wanted;
                if (m_multi)
                    curl_multi_wakeup(m_multi);
            }
            m_changed.wait_for(lock, std::chrono::milliseconds(50));
        }
        auto* output = static_cast<uint8_t*>(buffer);
        size_t copied = 0;
        while (copied < size) {
            off_t offset = position + copied;
            size_t index = offset / chunkSize;
            size_t within = offset % chunkSize;
            size_t length = std::min<size_t>(size - copied, chunkSize - within);
            std::memcpy(output + copied, m_chunks[index].get() + within, length);
            copied += length;
        }
        return copied;
    }

    ssize_t WriteAt(off_t, const void*, size_t) override { return B_NOT_ALLOWED; }
    off_t Seek(off_t position, uint32 mode) override
    {
        off_t base = mode == SEEK_CUR ? m_position : mode == SEEK_END ? m_size : 0;
        if (base + position < 0)
            return B_BAD_VALUE;
        m_position = base + position;
        return m_position;
    }
    off_t Position() const override { return m_position; }
    status_t GetSize(off_t* size) const override
    {
        *size = m_size;
        return B_OK;
    }
    status_t SetSize(off_t) override { return B_NOT_ALLOWED; }

private:
    StreamingMediaIO(std::string url, std::atomic<bool>& abort)
        : m_url(std::move(url))
        , m_abort(abort)
    {
    }

    bool complete(size_t index) const
    {
        if (index >= m_fill.size())
            return false;
        off_t expected = std::min<off_t>(chunkSize, m_size - off_t(index) * chunkSize);
        return m_fill[index] >= expected;
    }

    std::optional<off_t> nextStart()
    {
        if (m_wanted)
            return std::exchange(m_wanted, std::nullopt);
        for (size_t index = 0; index < m_fill.size(); ++index) {
            if (!complete(index))
                return off_t(index) * chunkSize;
        }
        return std::nullopt;
    }

    void run()
    {
        off_t start = 0;
        int failures = 0;
        while (true) {
            bool progressed = fetch(start);
            std::lock_guard lock(m_lock);
            if (m_stopping || m_abort)
                break;
            failures = progressed ? 0 : failures + 1;
            auto next = m_sizeKnown && failures < 3 ? nextStart() : std::nullopt;
            if (!next) {
                // Either everything is here or the server keeps failing.
                if (!m_sizeKnown || failures)
                    m_failed = true;
                m_changed.notify_all();
                break;
            }
            start = *next;
            if (!m_acceptsRanges && start) {
                m_failed = true;
                m_changed.notify_all();
                break;
            }
        }
    }

    // One request from start to the end of the file, or until a read wants
    // a part it will not reach soon. True when it delivered any bytes.
    bool fetch(off_t start)
    {
        struct Transfer {
            StreamingMediaIO& io;
            off_t start;
            off_t offset;
            long status { 0 };
            off_t total { -1 };
            off_t length { -1 };
            bool delivered { false };
        } transfer { *this, start, start };

        CURL* request = curl_easy_init();
        if (!request)
            return false;
        char range[64];
        std::snprintf(range, sizeof(range), "%lld-", static_cast<long long>(start));
        curl_easy_setopt(request, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(request, CURLOPT_RANGE, range);
        if (!m_userAgent.empty())
            curl_easy_setopt(request, CURLOPT_USERAGENT, m_userAgent.c_str());
        if (!m_referrer.empty())
            curl_easy_setopt(request, CURLOPT_REFERER, m_referrer.c_str());
        curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(request, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(request, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(request, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(request, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(request, CURLOPT_HEADERFUNCTION, +[](char* bytes, size_t size, size_t count, void* context) -> size_t {
            auto& transfer = *static_cast<Transfer*>(context);
            size_t length = size * count;
            std::string_view line(bytes, length);
            // Each response of a redirect chain starts with its status line.
            if (line.starts_with("HTTP/")) {
                auto space = line.find(' ');
                transfer.status = space == std::string_view::npos ? 0 : std::strtol(line.data() + space + 1, nullptr, 10);
                transfer.total = transfer.length = -1;
            } else if (line.size() > 15 && !strncasecmp(line.data(), "content-range:", 14)) {
                auto slash = line.find('/');
                if (slash != std::string_view::npos && line[slash + 1] != '*')
                    transfer.total = std::strtoll(line.data() + slash + 1, nullptr, 10);
            } else if (line.size() > 16 && !strncasecmp(line.data(), "content-length:", 15))
                transfer.length = std::strtoll(line.data() + 15, nullptr, 10);
            else if (line == "\r\n" || line == "\n") {
                if (transfer.status == 200 || transfer.status == 206)
                    transfer.io.didReceiveHeaders(transfer.status, transfer.status == 206 ? transfer.total : transfer.length);
            }
            return length;
        });
        curl_easy_setopt(request, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, +[](char* bytes, size_t size, size_t count, void* context) -> size_t {
            auto& transfer = *static_cast<Transfer*>(context);
            size_t length = size * count;
            // A server that ignores the range starts at the beginning again.
            if (transfer.status == 200 && transfer.start && transfer.offset == transfer.start)
                transfer.offset = 0;
            if (transfer.status != 200 && transfer.status != 206)
                return 0;
            if (!transfer.io.store(transfer.offset, bytes, length))
                return 0;
            transfer.offset += length;
            transfer.delivered = true;
            return length;
        });

        bool delivered = false;
        CURLM* multi = curl_multi_init();
        if (multi && curl_multi_add_handle(multi, request) == CURLM_OK) {
            {
                std::lock_guard lock(m_lock);
                m_multi = multi;
                m_streamStart = start;
                m_head = start;
            }
            int running = 0;
            CURLMcode result = curl_multi_perform(multi, &running);
            while (result == CURLM_OK && running) {
                {
                    std::lock_guard lock(m_lock);
                    if (m_stopping || m_abort || (m_wanted && (*m_wanted < m_streamStart || *m_wanted > m_head + reachAhead)))
                        break;
                }
                int descriptors = 0;
                result = curl_multi_poll(multi, nullptr, 0, 100, &descriptors);
                if (result == CURLM_OK)
                    result = curl_multi_perform(multi, &running);
            }
            {
                std::lock_guard lock(m_lock);
                m_multi = nullptr;
            }
            curl_multi_remove_handle(multi, request);
        }
        delivered = transfer.delivered;
        if (multi)
            curl_multi_cleanup(multi);
        curl_easy_cleanup(request);
        return delivered;
    }

    void didReceiveHeaders(long status, off_t size)
    {
        std::lock_guard lock(m_lock);
        if (m_sizeKnown)
            return;
        if (size > 0 && size <= maximumSize) {
            m_size = size;
            m_acceptsRanges = status == 206;
            size_t count = (size + chunkSize - 1) / chunkSize;
            m_chunks.resize(count);
            m_fill.assign(count, 0);
            m_sizeKnown = true;
        } else
            m_failed = true;
        m_changed.notify_all();
    }

    bool store(off_t offset, const char* bytes, size_t length)
    {
        std::lock_guard lock(m_lock);
        if (m_stopping || m_abort || !m_sizeKnown || offset + off_t(length) > m_size)
            return false;
        size_t written = 0;
        while (written < length) {
            off_t position = offset + written;
            size_t index = position / chunkSize;
            size_t within = position % chunkSize;
            size_t part = std::min<size_t>(length - written, chunkSize - within);
            if (!m_chunks[index])
                m_chunks[index].reset(new (std::nothrow) uint8_t[chunkSize]);
            if (!m_chunks[index])
                return false;
            std::memcpy(m_chunks[index].get() + within, bytes + written, part);
            // A request always starts on a chunk boundary and then runs on, so
            // the chunk holds everything up to here.
            m_fill[index] = std::max<uint32_t>(m_fill[index], within + part);
            written += part;
        }
        m_head = offset + length;
        m_changed.notify_all();
        return true;
    }

    const std::string m_url;
    std::string m_userAgent;
    std::string m_referrer;
    std::atomic<bool>& m_abort;
    off_t m_position { 0 };
    std::thread m_thread;

    mutable std::mutex m_lock;
    std::condition_variable m_changed;
    off_t m_size { -1 };
    bool m_sizeKnown { false };
    bool m_acceptsRanges { false };
    bool m_failed { false };
    bool m_stopping { false };
    std::vector<std::unique_ptr<uint8_t[]>> m_chunks;
    std::vector<uint32_t> m_fill;
    off_t m_streamStart { 0 };
    off_t m_head { 0 };
    std::optional<off_t> m_wanted;
    CURLM* m_multi { nullptr };
};

} // namespace WebCore
