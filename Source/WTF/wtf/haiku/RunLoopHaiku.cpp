/*
 * Copyright (C) 2014, 2024 Haiku, Inc
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/RunLoop.h>
#include <wtf/haiku/RunLoopHaiku.h>
#include <wtf/text/MakeString.h>

#include <Application.h>
#include <Autolock.h>
#include <Handler.h>
#include <MessageQueue.h>
#include <MessageRunner.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace WTF {

// BMessageRunner registers every timer with the registrar through synchronous
// messages, and cancels it the same way. WebKit arms and cancels timers constantly
// (every DOM timer, layout and IPC step), so those round trips dominated page work.
// One thread per process keeps the deadlines instead and posts the same native
// notification to the owning handler. Same-team delivery needs no port round trip.
class TimerScheduler {
public:
    static TimerScheduler& singleton()
    {
        static TimerScheduler* scheduler = new TimerScheduler;
        return *scheduler;
    }

    void schedule(const BMessenger& target, RunLoop::TimerBase* timer, int64 identifier, MonotonicTime fireTime)
    {
        {
            std::lock_guard locker(m_lock);
            removeWithLock(timer);
            Key key { fireTime, ++m_sequence };
            m_deadlines.emplace(key, Entry { target, timer, timer, identifier });
            m_keys.insert_or_assign(timer, key);
        }
        m_condition.notify_one();
    }

    // A wake-up that is dropped because the looper's port is full suspends
    // RunLoop::dispatch() for good: performWork() is what lifts the suspension,
    // and nothing posts again to a queue that is already non-empty. Retrying on
    // the caller's thread would deadlock when the caller is the looper itself,
    // so the deadline thread carries the retry instead.
    void scheduleWakeUp(const BMessenger& target, BHandler* handler)
    {
        {
            std::lock_guard locker(m_lock);
            if (m_keys.contains(handler))
                return;
            Key key { MonotonicTime::now() + 1_ms, ++m_sequence };
            m_deadlines.emplace(key, Entry { target, nullptr, handler, 0 });
            m_keys.insert_or_assign(handler, key);
        }
        m_condition.notify_one();
    }

    void cancel(RunLoop::TimerBase* timer)
    {
        std::lock_guard locker(m_lock);
        removeWithLock(timer);
    }

    void cancelWakeUp(BHandler* handler)
    {
        std::lock_guard locker(m_lock);
        removeWithLock(handler);
    }

    // describeTimerHaiku(): what this thread holds for a timer, and how it is.
    String describe(void* key)
    {
        std::lock_guard locker(m_lock);
        auto now = MonotonicTime::now();
        char entry[96] = "no deadline";
        if (auto found = m_keys.find(key); found != m_keys.end()) {
            auto deadline = m_deadlines.find(found->second);
            if (deadline == m_deadlines.end())
                std::snprintf(entry, sizeof(entry), "a key without its deadline");
            else
                std::snprintf(entry, sizeof(entry), "deadline %lld due in %.3f s", static_cast<long long>(deadline->second.identifier), (found->second.first - now).seconds());
        }
        char state[256];
        std::snprintf(state, sizeof(state), "%s; %zu deadlines, the next due in %.3f s; last awake %.3f s ago, %s; %llu failed posts",
            entry, m_deadlines.size(), m_deadlines.empty() ? 0.0 : (m_deadlines.begin()->first.first - now).seconds(),
            (now - m_lastAwake).seconds(), m_isWaiting ? "waiting" : "running", static_cast<unsigned long long>(m_failedPosts));
        return String::fromLatin1(state);
    }

    // The receiver rejects notifications for stopped, restarted or destroyed timers.
    // Answers whether the notification was delivered: a full port must not be
    // taken for a fired timer, because nothing would ever arm it again.
    static status_t post(const BMessenger& target, RunLoop::TimerBase* timer, int64 identifier)
    {
        BMessage message('tmrf');
        message.AddPointer("timer", timer);
        message.AddInt64("identifier", identifier);
        return target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }

private:
    using Key = std::pair<MonotonicTime, uint64_t>;
    struct Entry {
        BMessenger target;
        RunLoop::TimerBase* timer;
        void* key;
        int64 identifier;
    };

    static status_t post(const Entry& entry)
    {
        if (entry.timer)
            return post(entry.target, entry.timer, entry.identifier);
        BMessage message('loop');
        return entry.target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }

    TimerScheduler()
    {
        std::thread([this] { run(); }).detach();
    }

    void removeWithLock(void* key)
    {
        auto found = m_keys.find(key);
        if (found == m_keys.end())
            return;
        m_deadlines.erase(found->second);
        m_keys.erase(found);
    }

    void run()
    {
        rename_thread(find_thread(nullptr), "WebKit timers");
        std::unique_lock locker(m_lock);
        while (true) {
            m_lastAwake = MonotonicTime::now();
            if (m_deadlines.empty()) {
                m_isWaiting = true;
                m_condition.wait(locker);
                m_isWaiting = false;
                continue;
            }
            auto next = m_deadlines.begin();
            auto remaining = next->first.first - MonotonicTime::now();
            if (remaining > 0_s) {
                m_isWaiting = true;
                m_condition.wait_for(locker, std::chrono::microseconds(static_cast<int64_t>(std::min(remaining.microseconds(), 3600e6))));
                m_isWaiting = false;
                continue;
            }
            Entry entry = next->second;
            m_keys.erase(entry.key);
            m_deadlines.erase(next);
            locker.unlock();
            status_t posted = post(entry);
            locker.lock();
            if (posted != B_OK) {
                ++m_failedPosts;
                // The owner's port was full. Losing the notification would stop
                // this timer for good -- and the timer that stops is as likely
                // to be the one driving rendering updates as any other -- so
                // put the deadline back and try again shortly. Only if the
                // timer was armed again in the meantime does this one lapse.
                if (!m_keys.contains(entry.key)) {
                    Key retry { MonotonicTime::now() + 1_ms, ++m_sequence };
                    m_deadlines.emplace(retry, entry);
                    m_keys.insert_or_assign(entry.key, retry);
                }
            }
        }
    }

    std::mutex m_lock;
    std::condition_variable m_condition;
    std::map<Key, Entry> m_deadlines;
    std::unordered_map<void*, Key> m_keys;
    uint64_t m_sequence { 0 };
    MonotonicTime m_lastAwake;
    bool m_isWaiting { false };
    uint64_t m_failedPosts { 0 };
};

// BLooper::Loop() cannot be stopped without exiting its thread. A standalone
// JavaScript or worker run loop must instead return to its caller when stopped.
class StandaloneRunLoop final : public BLooper {
public:
    StandaloneRunLoop()
        : BLooper("WebKit RunLoop")
    {
    }

    void cycle(BHandler& handler, bigtime_t timeout)
    {
        std::unique_ptr<BMessage> message(MessageQueue()->NextMessage());
        if (!message)
            message.reset(MessageFromPort(timeout));
        if (!message)
            message.reset(MessageQueue()->NextMessage());
        if (message) {
            BAutolock locker(this);
            if (locker.IsLocked())
                handler.MessageReceived(message.get());
        }
    }
};

class LoopHandler final : public BHandler {
public:
    explicit LoopHandler(RunLoop& runLoop)
        : BHandler("RunLoop")
        , m_runLoop(runLoop)
    {
    }

    ~LoopHandler() override
    {
        Locker locker { timersLock };
        for (auto& [timer, state] : timers)
            TimerScheduler::singleton().cancel(timer);
        TimerScheduler::singleton().cancelWakeUp(this);
    }

    static void initialize(RunLoop& runLoop)
    {
        runLoop.m_handler = new LoopHandler(runLoop);
        auto thread = find_thread(nullptr);
        BLooper* looper = BLooper::LooperForThread(thread);
        // BApplication does not register its thread until Run(). Auxiliary
        // processes initialize WTF while the creating thread still owns the
        // application's initial lock, then drive this looper with Run().
        if (!looper && be_app && be_app->InitCheck() == B_OK
            && be_app->Thread() < B_OK && be_app->LockingThread() == thread)
            looper = be_app;
        if (!looper) {
            runLoop.m_looper = looper = new StandaloneRunLoop;
            looper->AddHandler(runLoop.m_handler);
            looper->Unlock();
        } else {
            BAutolock locker(looper);
            RELEASE_ASSERT(locker.IsLocked());
            looper->AddHandler(runLoop.m_handler);
        }
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == 'loop') {
            m_runLoop.performWork();
            return;
        }
        if (message->what == 'tmrf') {
            RunLoop::TimerBase* timer = nullptr;
            int64 identifier = 0;
            if (message->FindPointer("timer", reinterpret_cast<void**>(&timer)) != B_OK
                || message->FindInt64("identifier", &identifier) != B_OK)
                return;
            fireTimer(timer, identifier);
            return;
        }
        BHandler::MessageReceived(message);
    }

    // A queued native notification may outlive a stopped or destroyed timer,
    // or belong to an earlier start at the same memory address.
    void fireTimer(RunLoop::TimerBase* timer, int64 identifier)
    {
        {
            Locker locker { timersLock };
            ++received;
            auto found = timers.find(timer);
            if (found == timers.end()) {
                ++rejectedUnknown;
                return;
            }
            found->second.lastReceivedIdentifier = identifier;
            if (found->second.identifier != identifier) {
                ++rejectedStale;
                return;
            }
            ++firedCount;
            if (found->second.repeating) {
                found->second.fireTime = MonotonicTime::now() + found->second.interval;
                TimerScheduler::singleton().schedule(BMessenger(this), timer, identifier, found->second.fireTime);
            } else
                timers.erase(found);
        }
        timer->fired();
    }

    static String describe(const RunLoop::TimerBase& timer)
    {
        auto* handler = static_cast<LoopHandler*>(timer.m_runLoop->m_handler);
        char state[256] = "its run loop has stopped";
        if (handler) {
            Locker locker { handler->timersLock };
            auto found = handler->timers.find(const_cast<RunLoop::TimerBase*>(&timer));
            char posted[48] = "left to the timer thread";
            if (found != handler->timers.end() && found->second.postStatus != TimerState::notPosted)
                std::snprintf(posted, sizeof(posted), "posted with status 0x%x", static_cast<unsigned>(found->second.postStatus));
            if (found == handler->timers.end())
                std::snprintf(state, sizeof(state), "not armed");
            else
                std::snprintf(state, sizeof(state), "armed as %lld, due in %.3f s (%s %.3f s), %s, last notification for it %lld",
                    static_cast<long long>(found->second.identifier), (found->second.fireTime - MonotonicTime::now()).seconds(),
                    found->second.repeating ? "every" : "once after", found->second.interval.seconds(), posted,
                    static_cast<long long>(found->second.lastReceivedIdentifier));
            char loop[160];
            auto* looper = handler->Looper();
            std::snprintf(loop, sizeof(loop), "; its loop received %llu notifications: %llu fired, %llu stale, %llu for no armed timer; %d messages waiting",
                static_cast<unsigned long long>(handler->received), static_cast<unsigned long long>(handler->firedCount),
                static_cast<unsigned long long>(handler->rejectedStale), static_cast<unsigned long long>(handler->rejectedUnknown),
                looper ? static_cast<int>(looper->MessageQueue()->CountMessages()) : -1);
            return makeString(String::fromLatin1(state), String::fromLatin1(loop), "; timer thread: "_s,
                TimerScheduler::singleton().describe(const_cast<RunLoop::TimerBase*>(&timer)));
        }
        return makeString(String::fromLatin1(state), "; timer thread: "_s,
            TimerScheduler::singleton().describe(const_cast<RunLoop::TimerBase*>(&timer)));
    }

    struct TimerState {
        static constexpr status_t notPosted = 1;
        int64 identifier;
        MonotonicTime fireTime;
        Seconds interval;
        bool repeating;
        // For describe(): how a zero-delay start was posted, and the last
        // notification this loop received for the timer.
        status_t postStatus { notPosted };
        int64 lastReceivedIdentifier { 0 };
    };
    uint64_t received { 0 };
    uint64_t firedCount { 0 };
    uint64_t rejectedStale { 0 };
    uint64_t rejectedUnknown { 0 };
    // The timers have a lock of their own, not the looper's. The looper stays
    // locked while its thread handles a message, and other threads start and
    // stop its timers: JavaScriptCore does that for every VM's run loop under
    // one global lock, which its timers also take when they fire. A thread
    // arming a worker's timer then waited for the worker's looper while the
    // worker, firing a timer, waited for that lock; the page's main thread
    // followed on the same lock and the tab stopped (Proton's sign-in page,
    // whose crypto worker compiles WebAssembly). While it is held nothing is
    // called but the deadline thread's scheduling, which never waits on it.
    Lock timersLock;
    std::unordered_map<RunLoop::TimerBase*, TimerState> timers;
    int64 nextIdentifier { 0 };
    std::atomic<bool> running { false };
    std::atomic<bool> shouldStop { false };

private:
    RunLoop& m_runLoop;
};

RunLoop::RunLoop()
    : m_looper(nullptr)
    , m_handler(nullptr)
{
    // Give timers a valid native destination even before run() is entered.
    LoopHandler::initialize(*this);
}

RunLoop::~RunLoop()
{
    stop();
}

void RunLoop::run()
{
    auto& runLoop = currentSingleton();
    if (!runLoop.m_handler)
        LoopHandler::initialize(runLoop);
    if (!runLoop.m_looper)
        return; // BApplication or BWindow already dispatches this thread's messages.

    auto& handler = *static_cast<LoopHandler*>(runLoop.m_handler);
    RELEASE_ASSERT(!handler.running.exchange(true));
    handler.shouldStop = false;
    runLoop.wakeUp();
    while (!handler.shouldStop)
        static_cast<StandaloneRunLoop*>(runLoop.m_looper)->cycle(handler, B_INFINITE_TIMEOUT);
    handler.running = false;
}

void RunLoop::stop()
{
    auto* handler = static_cast<LoopHandler*>(m_handler);
    if (!handler)
        return;
    if (handler->running) {
        handler->shouldStop = true;
        wakeUp();
        return;
    }

    if (handler->LockLooper()) {
        BLooper* looper = handler->Looper();
        looper->RemoveHandler(handler);
        delete std::exchange(m_handler, nullptr);
        looper->Unlock();
    } else
        delete std::exchange(m_handler, nullptr);
    if (auto* looper = std::exchange(m_looper, nullptr)) {
        looper->Lock();
        looper->Quit(); // This looper never called BLooper::Run() or Loop().
    }
}

void RunLoop::wakeUp()
{
    if (!m_handler || !m_handler->Looper())
        return;
    if (m_handler->Looper()->PostMessage('loop', m_handler) == B_OK)
        return;
    TimerScheduler::singleton().scheduleWakeUp(BMessenger(m_handler), m_handler);
}

RunLoop::TimerBase::TimerBase(Ref<RunLoop>&& runLoop, ASCIILiteral description)
    : m_runLoop(WTF::move(runLoop))
    , m_description(description)
    , m_messageRunner(nullptr)
{
}

RunLoop::TimerBase::~TimerBase()
{
    stop();
}

void RunLoop::TimerBase::start(Seconds interval, bool repeat)
{
    auto* handler = static_cast<LoopHandler*>(m_runLoop->m_handler);
    if (!handler)
        return;
    auto fireTime = MonotonicTime::now() + interval;
    int64 identifier;
    {
        // A new identifier makes notifications of an earlier start stale.
        Locker locker { handler->timersLock };
        identifier = ++handler->nextIdentifier;
        LoopHandler::TimerState state { identifier, fireTime, interval, repeat };
        if (auto found = handler->timers.find(this); found != handler->timers.end())
            state.lastReceivedIdentifier = found->second.lastReceivedIdentifier;
        handler->timers.insert_or_assign(this, state);
    }
    // WebKit uses zero for work that should run on the next turn, including Promise
    // completion. Queue that directly behind the messages already waiting.
    // A zero interval means "next turn", which is a message queued behind the
    // ones already waiting. If the port cannot take it, fall back to the
    // scheduler rather than drop the timer.
    if (interval <= 0_s) {
        TimerScheduler::singleton().cancel(this);
        status_t posted = TimerScheduler::post(BMessenger(handler), this, identifier);
        {
            Locker locker { handler->timersLock };
            if (auto found = handler->timers.find(this); found != handler->timers.end() && found->second.identifier == identifier)
                found->second.postStatus = posted;
        }
        if (posted != B_OK)
            TimerScheduler::singleton().schedule(BMessenger(handler), this, identifier, fireTime);
    } else
        TimerScheduler::singleton().schedule(BMessenger(handler), this, identifier, fireTime);
}

bool RunLoop::TimerBase::isActive() const
{
    auto* handler = static_cast<LoopHandler*>(m_runLoop->m_handler);
    if (!handler)
        return false;
    Locker locker { handler->timersLock };
    return handler->timers.contains(const_cast<TimerBase*>(this));
}

void RunLoop::TimerBase::stop()
{
    if (auto* handler = static_cast<LoopHandler*>(m_runLoop->m_handler)) {
        Locker locker { handler->timersLock };
        handler->timers.erase(this);
    }
    TimerScheduler::singleton().cancel(this);
}

RunLoop::CycleResult RunLoop::cycle(RunLoopMode)
{
    auto& runLoop = currentSingleton();
    if (!runLoop.m_handler)
        return CycleResult::Stop;
    auto& handler = *static_cast<LoopHandler*>(runLoop.m_handler);
    bool wasRunning = handler.running.exchange(true);
    if (runLoop.m_looper)
        static_cast<StandaloneRunLoop*>(runLoop.m_looper)->cycle(handler, 0);
    else
        runLoop.performWork();
    handler.running = wasRunning;
    // IsMessageWaiting() requires the looper's lock. The application's looper
    // is locked by the thread running it; a standalone one (a worker thread's)
    // is only locked while it handles a message.
    BAutolock locker(handler.Looper());
    return locker.IsLocked() && handler.Looper()->IsMessageWaiting() ? CycleResult::Continue : CycleResult::Stop;
}

Seconds RunLoop::TimerBase::secondsUntilFire() const
{
    auto* handler = static_cast<LoopHandler*>(m_runLoop->m_handler);
    if (!handler)
        return 0_s;
    Locker locker { handler->timersLock };
    auto found = handler->timers.find(const_cast<TimerBase*>(this));
    return found == handler->timers.end() ? 0_s : std::max(0_s, found->second.fireTime - MonotonicTime::now());
}

String describeTimerHaiku(const RunLoop::TimerBase& timer)
{
    return LoopHandler::describe(timer);
}

} // namespace WTF
