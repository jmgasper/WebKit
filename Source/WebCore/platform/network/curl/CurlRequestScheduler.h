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

#pragma once

#include "CurlContext.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WTF {
class Thread;
}

namespace WebCore {

class CurlRequestSchedulerClient;

// Transfers are spread over several worker threads, each with its own multi
// handle, chosen by the request's host so that a host's connections (and its
// HTTP/2 streams) stay together and are reused. One thread polling every
// socket of every tab spent most of its time in the kernel and queued new
// requests for hundreds of milliseconds while dozens of pages loaded.
// SUMMIT_CURL_THREADS sets the count (default 4).
class CurlRequestScheduler {
    WTF_MAKE_TZONE_ALLOCATED(CurlRequestScheduler);
    WTF_MAKE_NONCOPYABLE(CurlRequestScheduler);
    friend NeverDestroyed<CurlRequestScheduler>;
public:
    CurlRequestScheduler(long maxConnects, long maxTotalConnections, long maxHostConnections);
    ~CurlRequestScheduler();

    bool add(CurlRequestSchedulerClient*, const String& host = { });
    void cancel(CurlRequestSchedulerClient*);

    // Runs on the thread that owns the client's transfer.
    void callOnWorkerThread(CurlRequestSchedulerClient*, Function<void()>&&);

private:
    class Worker {
        WTF_MAKE_TZONE_ALLOCATED_INLINE(Worker);
        WTF_MAKE_NONCOPYABLE(Worker);
    public:
        Worker(CurlRequestScheduler&, long maxConnects, long maxTotalConnections, long maxHostConnections);
        ~Worker();

        void add(CurlRequestSchedulerClient*);
        void cancel(CurlRequestSchedulerClient*);
        void callOnWorkerThread(Function<void()>&&);

    private:
        void startOrWakeUpThread();
        void wakeUpThreadIfPossible();
        void stopThreadIfNoMoreJobRunning();
        void stopThread();

        void executeTasks();

        void workerThread();

        void startTransfer(CurlRequestSchedulerClient*);
        void completeTransfer(CurlRequestSchedulerClient*, CURLcode);
        void cancelTransfer(CurlRequestSchedulerClient*);
        void finalizeTransfer(CurlRequestSchedulerClient*, Function<void()>);

        CurlRequestScheduler& m_scheduler;
        Lock m_mutex;
        RefPtr<WTF::Thread> m_thread;
        bool m_runThread { false };

        Vector<Function<void()>> m_taskQueue;
        HashSet<CurlRequestSchedulerClient*> m_activeJobs;
        HashMap<CURL*, CurlRequestSchedulerClient*> m_clientMaps;

        Lock m_multiHandleMutex;
        std::optional<CurlMultiHandle> m_curlMultiHandle;

        long m_maxConnects;
        long m_maxTotalConnections;
        long m_maxHostConnections;
    };

    Worker& workerForHost(const String& host);
    Worker* workerForClient(CurlRequestSchedulerClient*);
    void unassign(CurlRequestSchedulerClient*);

    Vector<std::unique_ptr<Worker>> m_workers;
    Lock m_assignmentLock;
    HashMap<CurlRequestSchedulerClient*, Worker*> m_assignments WTF_GUARDED_BY_LOCK(m_assignmentLock);
};

} // namespace WebCore
