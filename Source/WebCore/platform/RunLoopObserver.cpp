/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "RunLoopObserver.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RunLoopObserver);

RunLoopObserver::~RunLoopObserver()
{
    invalidate();
}

void RunLoopObserver::runLoopObserverFired()
{
#if USE(CF)
    ASSERT(m_runLoopObserver);
#elif USE(GLIB_EVENT_LOOP)
    if (ASSERT_ENABLED) {
        Locker locker { m_runLoopObserverLock };
        ASSERT(m_runLoopObserver);
    }
#endif
    m_callback();
}

#if PLATFORM(HAIKU)

// The coordinated renderers drive every rendering update through this observer,
// so a port without one paints nothing at all. CF and GLib run the callback as
// their loop goes idle.
//
// This has to be a timer rather than RunLoop::dispatch(). When the layer tree
// schedules the observer it also calls WindowEventLoop::breakToAllowRenderingUpdate(),
// which suspends function dispatch for the cycle so that the update happens
// before any more tasks run -- a dispatched observer would be deferred by the
// very thing that is waiting for it, and with an animation scheduling an update
// every cycle it would be deferred for good. Timers are not suspended.
void RunLoopObserver::schedule(PlatformRunLoop runLoop, OptionSet<Activity>)
{
    // The compositor thread schedules its did-composite observer on the main
    // run loop. Running the callback on whichever thread happened to arm it
    // would take the whole rendering update off the main thread, where the
    // timers it starts belong to a thread that never fires them.
    Ref target = runLoop ? Ref<RunLoop> { *runLoop } : Ref<RunLoop> { RunLoop::currentSingleton() };

    {
        Locker locker { m_timerLock };
        if (m_isScheduled)
            return;
        m_isScheduled = true;
    }

    // Arming a timer on another thread's loop takes that loop's BLooper lock,
    // which the thread being armed holds for as long as it is dispatching a
    // message -- including while it waits for this thread. Only the owning
    // thread may use the timer; everyone else dispatches, which needs no lock.
    // The suspension that the timer exists to avoid is lifted after one cycle,
    // so a dispatched observer is at worst one turn late.
    if (!target->isCurrent()) {
        target->dispatch([this] {
            {
                Locker locker { m_timerLock };
                if (!m_isScheduled)
                    return;
                m_isScheduled = false;
            }
            runLoopObserverFired();
        });
        return;
    }

    // Arming the timer takes the target loop's own lock, and the callback takes
    // this one while holding it, so this lock is never held across either call.
    RefPtr<RunLoop::DispatchTimer> timer = target->dispatchAfter(0_s, [this] {
        {
            Locker locker { m_timerLock };
            if (!m_isScheduled)
                return;
            m_isScheduled = false;
            m_timer = nullptr;
        }
        runLoopObserverFired();
    });

    // The timer may already have fired on the target thread by now, in which
    // case the flag says so and there is nothing left to keep.
    Locker locker { m_timerLock };
    if (m_isScheduled)
        m_timer = WTF::move(timer);
}

void RunLoopObserver::invalidate()
{
    RefPtr<RunLoop::DispatchTimer> timer;
    {
        Locker locker { m_timerLock };
        m_isScheduled = false;
        timer = std::exchange(m_timer, nullptr);
    }
    if (timer)
        timer->stop();
}

bool RunLoopObserver::isScheduled() const
{
    Locker locker { m_timerLock };
    return m_isScheduled;
}

#elif !USE(CF) && !USE(GLIB_EVENT_LOOP)

void RunLoopObserver::schedule(PlatformRunLoop, OptionSet<Activity>)
{
}

void RunLoopObserver::invalidate()
{
}

bool RunLoopObserver::isScheduled() const
{
    return false;
}

#endif // PLATFORM(HAIKU)

} // namespace WebCore
