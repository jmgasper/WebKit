/*
 * Copyright (C) 2019-2020 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
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
#include "RenderingUpdateScheduler.h"

#include "Chrome.h"
#include "ChromeClient.h"
#include "DisplayRefreshMonitorManager.h"
#include "Logging.h"
#include "Page.h"
#include "Timer.h"
#include <wtf/SystemTracing.h>
#if PLATFORM(HAIKU)
#include <wtf/text/MakeString.h>
#endif
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

#if PLATFORM(HAIKU)
#include <cstdlib>
#include <cstring>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RenderingUpdateScheduler);

RenderingUpdateScheduler::RenderingUpdateScheduler(Page& page)
    : m_page(page)
{
    windowScreenDidChange(page.chrome().displayID());
}

RenderingUpdateScheduler::~RenderingUpdateScheduler() = default;

bool RenderingUpdateScheduler::scheduleAnimation()
{
    if (m_useTimer)
        return false;

    return DisplayRefreshMonitorManager::sharedManager().scheduleAnimation(*this);
}

void RenderingUpdateScheduler::adjustRenderingUpdateFrequency()
{
    auto renderingUpdateFramesPerSecond = protect(m_page)->preferredRenderingUpdateFramesPerSecond();
    if (renderingUpdateFramesPerSecond) {
        setPreferredFramesPerSecond(renderingUpdateFramesPerSecond.value());
        m_useTimer = false;
    } else
        m_useTimer = true;

    if (m_refreshTimer) {
        clearScheduled();
        scheduleRenderingUpdate();
    }
}

void RenderingUpdateScheduler::scheduleRenderingUpdate()
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " scheduleTimedRenderingUpdate() - already scheduled " << isScheduled() << " page visible " << m_page->isVisible());

    if (isScheduled())
        return;

    // Optimize the case when an invisible page wants just to schedule layer flush.
    if (!m_page->isVisible()) {
        triggerRenderingUpdate();
        return;
    }

    tracePoint(ScheduleRenderingUpdate);

    if (!scheduleAnimation()) {
        LOG_WITH_STREAM(DisplayLink, stream << "RenderingUpdateScheduler::scheduleRenderingUpdate for interval " << protect(m_page)->preferredRenderingUpdateInterval() << " falling back to timer");
        Seconds delay = protect(m_page)->preferredRenderingUpdateInterval();
#if PLATFORM(HAIKU)
        // Haiku has no display link, so every update comes from this timer.
        // Counted from the request, the interval came on top of the time the
        // page took to ask (requestAnimationFrame from inside its callback):
        // a page asking 2 ms into its frame got a frame every 19 ms, 52 a
        // second (WebGL Aquarium). Count it from the last update's start, as a
        // display's refresh would; an update that is overdue runs at once.
        // SUMMIT_REFRESH_FROM_REQUEST=1 returns to the old timing.
        static const bool fromRequest = [] {
            const char* value = getenv("SUMMIT_REFRESH_FROM_REQUEST");
            return value && !strcmp(value, "1");
        }();
        if (!fromRequest && m_lastRefreshHaiku)
            delay = std::clamp(delay - (MonotonicTime::now() - m_lastRefreshHaiku), 0_s, delay);
#endif
        startTimer(delay);
    }
    
    protect(m_page)->didScheduleRenderingUpdate();
}

bool RenderingUpdateScheduler::isScheduled() const
{
    return m_refreshTimer || DisplayRefreshMonitorClient::isScheduled();
}
    
void RenderingUpdateScheduler::startTimer(Seconds delay)
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " startTimer(" << delay << ")");

    ASSERT(!m_refreshTimer);
    m_refreshTimer = makeUnique<Timer>(*this, &RenderingUpdateScheduler::displayRefreshFired);
    m_refreshTimer->startOneShot(delay);
}

void RenderingUpdateScheduler::clearScheduled()
{
    m_refreshTimer = nullptr;
}

#if PLATFORM(HAIKU)
String RenderingUpdateScheduler::describeHaiku() const
{
    if (!isScheduled())
        return "idle"_s;
    if (!m_refreshTimer)
        return "pending through a display refresh monitor"_s;
    if (!m_refreshTimer->isActive())
        return "pending on a refresh timer that is not running"_s;
    return makeString("refresh timer due in "_s, m_refreshTimer->nextFireInterval().milliseconds(), " ms"_s);
}

void RenderingUpdateScheduler::restartHaiku()
{
    clearScheduled();
    triggerRenderingUpdate();
}
#endif

DisplayRefreshMonitorFactory* RenderingUpdateScheduler::displayRefreshMonitorFactory() const
{
    return m_page->chrome().client().displayRefreshMonitorFactory();
}

void RenderingUpdateScheduler::windowScreenDidChange(PlatformDisplayID displayID)
{
    adjustRenderingUpdateFrequency();
    DisplayRefreshMonitorManager::sharedManager().windowScreenDidChange(displayID, *this);
}

void RenderingUpdateScheduler::displayRefreshFired()
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " displayRefreshFired()");

    tracePoint(TriggerRenderingUpdate);

#if PLATFORM(HAIKU)
    m_lastRefreshHaiku = MonotonicTime::now();
#endif
    clearScheduled();
    
    if (m_page->chrome().client().shouldTriggerRenderingUpdate(m_rescheduledRenderingUpdateCount)) {
        triggerRenderingUpdate();
        m_rescheduledRenderingUpdateCount = 0;
    } else {
        scheduleRenderingUpdate();
        ++m_rescheduledRenderingUpdateCount;
    }
}

void RenderingUpdateScheduler::triggerRenderingUpdate()
{
    m_page->chrome().client().triggerRenderingUpdate();
}

} // namespace WebCore
