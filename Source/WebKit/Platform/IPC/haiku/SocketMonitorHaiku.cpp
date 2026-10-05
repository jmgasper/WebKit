/*
 * Copyright (C) 2026 KunanyiOS contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SocketMonitorHaiku.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <wtf/Threading.h>
#include <wtf/UniStdExtras.h>
#include <wtf/unix/UnixFileDescriptor.h>

namespace IPC {

struct SocketMonitorHaiku::State : ThreadSafeRefCounted<State> {
    State(UnixFileDescriptor&& socket, UnixFileDescriptor&& wakeRead, UnixFileDescriptor&& wakeWrite,
        WorkQueue& queue, Function<void()>&& callback, Mode mode)
        : socket(WTF::move(socket))
        , wakeRead(WTF::move(wakeRead))
        , wakeWrite(WTF::move(wakeWrite))
        , queue(queue)
        , callback(WTF::move(callback))
        , events(mode == Mode::Read ? POLLIN : POLLOUT)
    {
    }

    void wake()
    {
        char byte = 0;
        // A full nonblocking pipe already supplies the needed wakeup.
        while (write(wakeWrite.value(), &byte, 1) < 0 && errno == EINTR) { }
    }

    void drainWakeups()
    {
        char bytes[64];
        while (true) {
            auto count = read(wakeRead.value(), bytes, sizeof(bytes));
            if (count > 0 || (count < 0 && errno == EINTR))
                continue;
            return;
        }
    }

    void run()
    {
        while (active.load()) {
            // While a callback is queued/running, wait only for its acknowledgement
            // or stop(). This bounds the queue even if the peer remains readable.
            pollfd descriptors[] = {
                { callbackPending.load() ? -1 : socket.value(), events, 0 },
                { wakeRead.value(), POLLIN, 0 }
            };
            int result;
            do {
                result = poll(descriptors, 2, -1);
            } while (result < 0 && errno == EINTR);
            if (!active.load())
                return;
            if (descriptors[1].revents)
                drainWakeups();
            if (result >= 0 && !descriptors[0].revents)
                continue;

            callbackPending.store(true);
            queue->dispatch([state = Ref { *this }] {
                if (state->active.load())
                    state->callback();
                state->callbackPending.store(false);
                state->wake();
            });
            if (result < 0)
                return;
        }
    }

    // Duplicate the watched socket: closing/reusing the caller's descriptor
    // cannot make this thread accidentally monitor an unrelated resource.
    UnixFileDescriptor socket;
    UnixFileDescriptor wakeRead;
    UnixFileDescriptor wakeWrite;
    Ref<WorkQueue> queue;
    Function<void()> callback;
    short events;
    std::atomic<bool> active { true };
    std::atomic<bool> callbackPending { false };
};

std::unique_ptr<SocketMonitorHaiku> SocketMonitorHaiku::create(int descriptor, WorkQueue& queue, Function<void()>&& callback, Mode mode)
{
    assertIsCurrent(queue);
    UnixFileDescriptor socket { descriptor, UnixFileDescriptor::Duplicate };
    if (!socket)
        return nullptr;
    int wakeDescriptors[2];
    // Set close-on-exec atomically so a concurrent process launch cannot
    // inherit a wakeup endpoint between creation and fcntl().
    if (pipe2(wakeDescriptors, O_CLOEXEC | O_NONBLOCK) < 0)
        return nullptr;
    UnixFileDescriptor wakeRead { wakeDescriptors[0], UnixFileDescriptor::Adopt };
    UnixFileDescriptor wakeWrite { wakeDescriptors[1], UnixFileDescriptor::Adopt };
    return std::unique_ptr<SocketMonitorHaiku>(new SocketMonitorHaiku(adoptRef(*new State(
        WTF::move(socket), WTF::move(wakeRead), WTF::move(wakeWrite), queue, WTF::move(callback), mode))));
}

SocketMonitorHaiku::SocketMonitorHaiku(Ref<State>&& state)
    : m_state(WTF::move(state))
    , m_thread(Thread::create("IPC socket monitor"_s, [state = Ref { *m_state }] { state->run(); }))
{
}

SocketMonitorHaiku::~SocketMonitorHaiku()
{
    stop();
}

void SocketMonitorHaiku::stop()
{
    if (!m_state)
        return;
    assertIsCurrent(m_state->queue);
    m_state->active.store(false);
    m_state->wake();
    m_thread->waitForCompletion();
    m_thread = nullptr;
    // An already queued callback retains State and sees active=false. If stop()
    // is called inside the callback, that same reference preserves its Function
    // until invocation returns; we never destroy a Function while calling it.
    m_state = nullptr;
}

} // namespace IPC
