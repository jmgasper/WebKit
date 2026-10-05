/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
 * Copyright (C) 2017 Sony Interactive Entertainment Inc.
 * Copyright (C) 2017 NAVER Corp.
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
#include "CurlRequestScheduler.h"

#include <cstdlib>
#include <cstring>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/MonotonicTime.h>
#include <cstdio>

#if USE(CURL)

#include "CurlRequestSchedulerClient.h"

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CurlRequestScheduler);

static size_t workerCount()
{
    size_t count = 4;
    if (const char* value = std::getenv("SUMMIT_CURL_THREADS")) {
        auto parsed = std::strtol(value, nullptr, 10);
        if (parsed >= 1 && parsed <= 16)
            count = parsed;
    }
    return count;
}

CurlRequestScheduler::CurlRequestScheduler(long maxConnects, long maxTotalConnections, long maxHostConnections)
{
    auto count = workerCount();
    m_workers.reserveInitialCapacity(count);
    for (size_t index = 0; index < count; ++index)
        m_workers.append(makeUnique<Worker>(*this, maxConnects, maxTotalConnections, maxHostConnections));
}

CurlRequestScheduler::~CurlRequestScheduler()
{
    m_workers.clear();
}

CurlRequestScheduler::Worker& CurlRequestScheduler::workerForHost(const String& host)
{
    return *m_workers[host.hash() % m_workers.size()];
}

CurlRequestScheduler::Worker* CurlRequestScheduler::workerForClient(CurlRequestSchedulerClient* client)
{
    Locker locker { m_assignmentLock };
    return m_assignments.get(client);
}

void CurlRequestScheduler::unassign(CurlRequestSchedulerClient* client)
{
    Locker locker { m_assignmentLock };
    m_assignments.remove(client);
}

bool CurlRequestScheduler::add(CurlRequestSchedulerClient* client, const String& host)
{
    ASSERT(isMainThread());

    if (!client)
        return false;

    Worker& worker = workerForHost(host);
    {
        Locker locker { m_assignmentLock };
        m_assignments.set(client, &worker);
    }
    worker.add(client);
    return true;
}

void CurlRequestScheduler::cancel(CurlRequestSchedulerClient* client)
{
    ASSERT(isMainThread());

    if (!client)
        return;

    if (auto* worker = workerForClient(client))
        worker->cancel(client);
}

void CurlRequestScheduler::callOnWorkerThread(CurlRequestSchedulerClient* client, Function<void()>&& task)
{
    auto* worker = client ? workerForClient(client) : nullptr;
    (worker ? *worker : *m_workers[0]).callOnWorkerThread(WTF::move(task));
}

// Worker: one multi handle and the thread that drives it -----------------

CurlRequestScheduler::Worker::Worker(CurlRequestScheduler& scheduler, long maxConnects, long maxTotalConnections, long maxHostConnections)
    : m_scheduler(scheduler)
    , m_maxConnects(maxConnects)
    , m_maxTotalConnections(maxTotalConnections)
    , m_maxHostConnections(maxHostConnections)
{
}

CurlRequestScheduler::Worker::~Worker()
{
    stopThread();
}

void CurlRequestScheduler::Worker::add(CurlRequestSchedulerClient* client)
{
    ASSERT(isMainThread());

    startTransfer(client);
    startOrWakeUpThread();
}

void CurlRequestScheduler::Worker::cancel(CurlRequestSchedulerClient* client)
{
    ASSERT(isMainThread());

    cancelTransfer(client);
}

void CurlRequestScheduler::Worker::callOnWorkerThread(Function<void()>&& task)
{
    {
        Locker locker { m_mutex };
        m_taskQueue.append(WTF::move(task));
    }

    startOrWakeUpThread();
}

void CurlRequestScheduler::Worker::startOrWakeUpThread()
{
    ASSERT(isMainThread());

    {
        Locker locker { m_mutex };
        if (m_runThread) {
            wakeUpThreadIfPossible();
            return;
        }
    }

    if (m_thread)
        m_thread->waitForCompletion();

    {
        Locker locker { m_mutex };
        m_runThread = true;
    }

    m_thread = Thread::create("curlThread"_s, [this] {
        workerThread();
    }, ThreadType::Network);
}

void CurlRequestScheduler::Worker::wakeUpThreadIfPossible()
{
    Locker locker { m_multiHandleMutex };
    if (!m_curlMultiHandle)
        return;

    m_curlMultiHandle->wakeUp();
}

void CurlRequestScheduler::Worker::stopThreadIfNoMoreJobRunning()
{
    ASSERT(!isMainThread());

    Locker locker { m_mutex };
    if (m_activeJobs.size() || m_taskQueue.size())
        return;

    m_runThread = false;
}

void CurlRequestScheduler::Worker::stopThread()
{
    {
        Locker locker { m_mutex };
        m_runThread = false;
    }

    if (m_thread) {
        wakeUpThreadIfPossible();
        m_thread->waitForCompletion();
        m_thread = nullptr;
    }
}

void CurlRequestScheduler::Worker::executeTasks()
{
    ASSERT(!isMainThread());

    Vector<Function<void()>> taskQueue;

    {
        Locker locker { m_mutex };
        taskQueue = WTF::move(m_taskQueue);
    }

    for (auto& task : taskQueue)
        task();
}

void CurlRequestScheduler::Worker::workerThread()
{
    ASSERT(!isMainThread());

    {
        Locker locker { m_multiHandleMutex };
        m_curlMultiHandle.emplace();
        m_curlMultiHandle->setMaxConnects(m_maxConnects);
        m_curlMultiHandle->setMaxTotalConnections(m_maxTotalConnections);
        m_curlMultiHandle->setMaxHostConnections(m_maxHostConnections);
    }

    // SUMMIT_NET_TRACE=1 reports loop iterations that kept queued requests
    // waiting: how long the tasks, the poll, perform and the completions took.
    static const bool traceIterations = [] {
        const char* value = getenv("SUMMIT_NET_TRACE");
        return value && value[0] == '1' && !value[1];
    }();

    while (true) {
        {
            Locker locker { m_mutex };
            if (!m_runThread)
                break;
        }

        auto iterationStart = MonotonicTime::now();
        size_t taskCount = 0;
        {
            Locker locker { m_mutex };
            taskCount = m_taskQueue.size();
        }
        executeTasks();
        auto tasksDone = MonotonicTime::now();

        const int selectTimeoutMS = INT_MAX;
        m_curlMultiHandle->poll({ }, selectTimeoutMS);
        auto pollDone = MonotonicTime::now();

        int activeCount = 0;
        while (m_curlMultiHandle->perform(activeCount) == CURLM_CALL_MULTI_PERFORM) { }
        auto performDone = MonotonicTime::now();

        // check the curl messages indicating completed transfers
        // and free their resources
        unsigned completions = 0;
        while (true) {
            int messagesInQueue = 0;
            CURLMsg* msg = m_curlMultiHandle->readInfo(messagesInQueue);
            if (!msg)
                break;

            ASSERT(msg->msg == CURLMSG_DONE);
            ++completions;
            if (auto client = m_clientMaps.inlineGet(msg->easy_handle))
                completeTransfer(client, msg->data.result);
        }

        if (traceIterations) {
            auto end = MonotonicTime::now();
            auto busy = (end - iterationStart) - (pollDone - tasksDone);
            if (busy > 50_ms) {
                fprintf(stderr, "Summit net: worker %p slow iteration: tasks %zu in %.0f ms, poll %.0f ms, perform %.0f ms (%d active), %u completions in %.0f ms\n",
                    this, taskCount, (tasksDone - iterationStart).milliseconds(), (pollDone - tasksDone).milliseconds(),
                    (performDone - pollDone).milliseconds(), activeCount, completions, (end - performDone).milliseconds());
            }
        }

//        stopThreadIfNoMoreJobRunning();
    }

    {
        Locker locker { m_multiHandleMutex };
        m_curlMultiHandle.reset();
    }
}

void CurlRequestScheduler::Worker::startTransfer(CurlRequestSchedulerClient* client)
{
    client->retain();

    auto task = [this, client]() {
        CURL* handle = client->setupTransfer();
        if (!handle) {
            completeTransfer(client, CURLE_FAILED_INIT);
            return;
        }

        m_curlMultiHandle->addHandle(handle);

        ASSERT(!m_clientMaps.contains(handle));
        m_clientMaps.set(handle, client);
    };

    Locker locker { m_mutex };
    m_activeJobs.add(client);
    m_taskQueue.append(WTF::move(task));
}

void CurlRequestScheduler::Worker::completeTransfer(CurlRequestSchedulerClient* client, CURLcode result)
{
    finalizeTransfer(client, [client, result]() {
        client->didCompleteTransfer(result);
    });
}

void CurlRequestScheduler::Worker::cancelTransfer(CurlRequestSchedulerClient* client)
{
    finalizeTransfer(client, [client]() {
        client->didCancelTransfer();
    });
}

void CurlRequestScheduler::Worker::finalizeTransfer(CurlRequestSchedulerClient* client, Function<void()> completionHandler)
{
    {
        Locker locker { m_mutex };

        if (!m_activeJobs.contains(client))
            return;

        m_activeJobs.remove(client);

        auto task = [this, client, completionHandler = WTF::move(completionHandler)]() {
            if (client->handle()) {
                ASSERT(m_clientMaps.contains(client->handle()));
                m_clientMaps.remove(client->handle());
                m_curlMultiHandle->removeHandle(client->handle());
            }

            completionHandler();

            callOnMainThread([client]() {
                client->release();
            });
        };

        m_taskQueue.append(WTF::move(task));
    }
    // Outside m_mutex: the scheduler's lock is never taken under a worker's.
    m_scheduler.unassign(client);
}

}

#endif
