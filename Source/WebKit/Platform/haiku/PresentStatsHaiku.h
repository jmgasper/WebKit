/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <OS.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace WebKit {

// SUMMIT_PRESENT_STATS=1 prints, every two seconds, how long each stage that
// moves a finished frame towards the screen took: the web process reading it
// back from the GPU, the UI process copying it, app_server drawing it. Each
// stage is a full-frame copy, so this is where a high density screen costs.
class PresentStageStatsHaiku {
public:
    explicit PresentStageStatsHaiku(const char* name)
        : m_name(name)
    {
    }

    static bool enabled()
    {
        static const bool on = [] {
            const char* value = std::getenv("SUMMIT_PRESENT_STATS");
            return value && !std::strcmp(value, "1");
        }();
        return on;
    }

    void add(bigtime_t elapsed, double megapixels)
    {
        std::lock_guard guard(m_mutex);
        bigtime_t now = system_time();
        if (!m_windowStart)
            m_windowStart = now;
        ++m_count;
        m_total += elapsed;
        m_max = std::max(m_max, elapsed);
        m_megapixels += megapixels;
        if (now - m_windowStart < 2000000)
            return;
        std::fprintf(stderr, "Summit present %s: %ld in %.1f s, mean %.2f ms, max %.2f ms, %.2f Mpx each\n",
            m_name, long(m_count), (now - m_windowStart) / 1e6, m_total / 1e3 / m_count, m_max / 1e3, m_megapixels / m_count);
        m_windowStart = now;
        m_count = 0;
        m_total = 0;
        m_max = 0;
        m_megapixels = 0;
    }

private:
    const char* m_name;
    std::mutex m_mutex;
    bigtime_t m_windowStart { 0 };
    long m_count { 0 };
    bigtime_t m_total { 0 };
    bigtime_t m_max { 0 };
    double m_megapixels { 0 };
};

} // namespace WebKit
