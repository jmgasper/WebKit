/*
 * Copyright (C) 2026 Haiku, Inc.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS AS IS''
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
#include "MemoryPressureMonitorHaiku.h"

#include "WebProcessPool.h"
#include <OS.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>

namespace WebKit {

// The Linux monitor's policy: poll every 5 s while less than half the memory
// is in use, down to every second at 85%; memory pressure from 90%, critical
// from 95%. SUMMIT_MEMORY_PRESSURE_PERCENT=<n> moves the first threshold (and
// the critical one with it) to test the response on a machine with plenty.
static const Seconds s_minPollingInterval { 1_s };
static const Seconds s_maxPollingInterval { 5_s };
static const double s_minUsedMemoryPercentageForPolling = 50;
static const double s_maxUsedMemoryPercentageForPolling = 85;

static int pressureThreshold()
{
    static int threshold = [] {
        const char* value = getenv("SUMMIT_MEMORY_PRESSURE_PERCENT");
        int parsed = value ? atoi(value) : 0;
        return parsed > 0 && parsed <= 100 ? parsed : 90;
    }();
    return threshold;
}

// Pages the file cache holds count as available: the kernel drops them when
// something needs the memory.
static int systemMemoryUsedAsPercentage()
{
    system_info info;
    if (get_system_info(&info) != B_OK || !info.max_pages)
        return -1;
    return static_cast<int>(100.0 * info.used_pages / info.max_pages);
}

static Seconds pollIntervalForUsedMemoryPercentage(int usedPercentage)
{
    if (usedPercentage < s_minUsedMemoryPercentageForPolling)
        return s_maxPollingInterval;
    if (usedPercentage >= s_maxUsedMemoryPercentageForPolling)
        return s_minPollingInterval;
    return s_minPollingInterval + (s_maxPollingInterval - s_minPollingInterval)
        * ((s_maxUsedMemoryPercentageForPolling - usedPercentage) / (s_maxUsedMemoryPercentageForPolling - s_minUsedMemoryPercentageForPolling));
}

MemoryPressureMonitor& MemoryPressureMonitor::singleton()
{
    static NeverDestroyed<MemoryPressureMonitor> memoryMonitor;
    return memoryMonitor;
}

void MemoryPressureMonitor::start()
{
    if (m_started)
        return;
    m_started = true;

    Thread::create("MemoryPressureMonitor"_s, [] {
        Seconds pollInterval = s_maxPollingInterval;
        int reportedLevel = 0;
        while (true) {
            sleep(pollInterval);
            int usedPercentage = systemMemoryUsedAsPercentage();
            if (usedPercentage < 0) {
                pollInterval = s_maxPollingInterval;
                continue;
            }
            int threshold = pressureThreshold();
            int level = usedPercentage >= std::min(threshold + 5, 100) ? 2 : usedPercentage >= threshold ? 1 : 0;
            if (level != reportedLevel) {
                std::fprintf(stderr, "Summit memory pressure: %d%% of memory in use, %s\n", usedPercentage,
                    level == 2 ? "critical" : level == 1 ? "releasing memory" : "normal");
                reportedLevel = level;
            }
            if (level) {
                bool isCritical = level == 2;
                RunLoop::mainSingleton().dispatch([isCritical] {
                    for (auto& processPool : WebProcessPool::allProcessPools())
                        processPool->sendMemoryPressureEvent(isCritical);
                });
            }
            pollInterval = pollIntervalForUsedMemoryPercentage(usedPercentage);
        }
    })->detach();
}

bool MemoryPressureMonitor::disabled()
{
    static bool disabled = [] {
        const char* value = getenv("WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR");
        return value && !strcmp(value, "1");
    }();
    return disabled;
}

} // namespace WebKit
