/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "IPCEvent.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace IPC {

bool Signal::isValidConnection(const UnixFileDescriptor& connection)
{
    if (!connection)
        return false;
    int type;
    socklen_t size = sizeof(type);
    if (getsockopt(connection.value(), SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM)
        return false;
    sockaddr_un peer { };
    size = sizeof(peer);
    return !getpeername(connection.value(), reinterpret_cast<sockaddr*>(&peer), &size) && peer.sun_family == AF_UNIX;
}

std::optional<EventSignalPair> createEventSignalPair()
{
    Semaphore event;
    if (!event)
        return std::nullopt;
    Semaphore signal(event.duplicateDescriptor());
    if (!signal)
        return std::nullopt;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
        return std::nullopt;
    // No payload ever enters the socket. It exists only to report the death of
    // the last Signal owner, including ownership transferred through SCM_RIGHTS.
    return EventSignalPair {
        Event { WTF::move(event), UnixFileDescriptor { sockets[0], UnixFileDescriptor::Adopt } },
        Signal { WTF::move(signal), UnixFileDescriptor { sockets[1], UnixFileDescriptor::Adopt } }
    };
}

bool Event::wait()
{
    return waitFor(Timeout::infinity());
}

bool Event::waitFor(Timeout timeout)
{
    if (m_peerDied || !m_connection || !m_semaphore)
        return false;
    for (;;) {
        if (!m_semaphore)
            return false;
        // Preserve signals published before the final peer endpoint closed.
        if (m_semaphore.waitFor(Timeout::now()))
            return true;
        pollfd descriptor { m_connection.value(), POLLIN, 0 };
        int result;
        do {
            result = poll(&descriptor, 1, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0 || (result && descriptor.revents)) {
            m_peerDied = true;
            // The sender may have published its final count between the first
            // check and EOF. Drain it before making peer death permanent.
            if (m_semaphore.waitFor(Timeout::now())) {
                m_peerDied = false;
                return true;
            }
            return false;
        }
        if (!timeout.isInfinity() && timeout.didTimeOut())
            return false;
        auto interval = timeout.isInfinity() ? 100_ms : std::min(timeout.secondsUntilDeadline(), 100_ms);
        if (m_semaphore.waitFor(Timeout { interval }))
            return true;
    }
}

} // namespace IPC
