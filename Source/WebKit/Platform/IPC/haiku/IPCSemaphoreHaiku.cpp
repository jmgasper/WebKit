/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "IPCSemaphore.h"
#include "IPCSemaphoreSharedHaiku.h"

#include <WebCore/SharedMemory.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <semaphore.h>
#include <sys/stat.h>
#include <time.h>
#include <wtf/StdLibExtras.h>

namespace IPC {

namespace {

static SharedSemaphoreHaiku& sharedState(WebCore::SharedMemory& memory)
{
    return *reinterpret_cast<SharedSemaphoreHaiku*>(memory.mutableSpan().data());
}

static bool validState(const SharedSemaphoreHaiku& state)
{
    static const int32_t expectedType = [] {
        sem_t expected { };
        RELEASE_ASSERT(!sem_init(&expected, 1, 0));
        auto type = expected.type;
        sem_destroy(&expected);
        return type;
    }();
    // libroot/kernel accesses this word atomically. Only -1 (contended), 0,
    // and 1 are reachable with our single outstanding wake notification.
    auto wakeCount = std::atomic_ref<const int32_t>(state.wake.u.unnamed_sem).load(std::memory_order_relaxed);
    return state.magic == SharedSemaphoreHaiku::expectedMagic && state.version == 1
        && state.semaphoreSize == sizeof(sem_t) && !state.reserved
        && state.wake.type == expectedType && wakeCount >= -1 && wakeCount <= 1
        && state.wakePending.load(std::memory_order_relaxed) <= 1
        && state.count.load(std::memory_order_relaxed) != std::numeric_limits<uint64_t>::max();
}

static RefPtr<WebCore::SharedMemory> mapDescriptor(UnixFileDescriptor&& descriptor)
{
    if (!descriptor)
        return nullptr;
    struct stat info;
    if (fstat(descriptor.value(), &info) || !S_ISREG(info.st_mode)
        || info.st_size != sizeof(SharedSemaphoreHaiku)
        || (fcntl(descriptor.value(), F_GETFL) & O_ACCMODE) != O_RDWR)
        return nullptr;
    auto memory = WebCore::SharedMemory::map({ WTF::move(descriptor), sizeof(SharedSemaphoreHaiku) }, WebCore::SharedMemory::Protection::ReadWrite);
    if (!memory)
        return nullptr;
    return validState(sharedState(*memory)) ? memory : nullptr;
}

static void wakeWaiter(SharedSemaphoreHaiku& state)
{
    if (!state.wakePending.exchange(1, std::memory_order_acq_rel) && sem_post(&state.wake))
        WTFLogAlways("IPC::Semaphore: native wake failed (%d); the shared count remains pending", errno);
}

static bool tryAcquire(SharedSemaphoreHaiku& state)
{
    auto count = state.count.load(std::memory_order_acquire);
    while (count) {
        if (count == std::numeric_limits<uint64_t>::max()) {
            WTFLogAlways("IPC::Semaphore: rejected corrupted shared count");
            return false;
        }
        if (state.count.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (count > 1)
                wakeWaiter(state);
            return true;
        }
    }
    return false;
}

static timespec wakeDeadline(Timeout timeout)
{
    timespec deadline;
    RELEASE_ASSERT(!clock_gettime(CLOCK_MONOTONIC, &deadline));
    // Recheck the count even if a peer dies after publishing it but before
    // sem_post. This also makes an interrupted notification harmless.
    double seconds = timeout.isInfinity() ? 0.1 : std::min(timeout.secondsUntilDeadline().seconds(), 0.1);
    auto nanoseconds = static_cast<long>(std::ceil(std::max(seconds, 0.0) * 1000000000.0));
    deadline.tv_nsec += nanoseconds;
    if (deadline.tv_nsec >= 1000000000) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    return deadline;
}

} // namespace

Semaphore::Semaphore()
    : m_memory(WebCore::SharedMemory::allocate(sizeof(SharedSemaphoreHaiku)))
{
    if (!m_memory)
        return;
    auto* state = new (m_memory->mutableSpan().data()) SharedSemaphoreHaiku;
    if (sem_init(&state->wake, 1, 0))
        m_memory = nullptr;
}

Semaphore::Semaphore(UnixFileDescriptor&& descriptor)
    : m_memory(mapDescriptor(WTF::move(descriptor)))
{
}

Semaphore::Semaphore(Semaphore&&) = default;
Semaphore& Semaphore::operator=(Semaphore&&) = default;

Semaphore::~Semaphore()
{
    destroy();
}

Semaphore::operator bool() const
{
    return m_memory && validState(sharedState(*m_memory));
}

bool Semaphore::isValidDescriptor(const UnixFileDescriptor& descriptor)
{
    return !!mapDescriptor(descriptor.duplicate());
}

UnixFileDescriptor Semaphore::duplicateDescriptor() const
{
    if (!m_memory)
        return { };
    auto handle = m_memory->createHandle(WebCore::SharedMemory::Protection::ReadWrite);
    return handle ? handle->releaseHandle() : UnixFileDescriptor { };
}

void Semaphore::signal()
{
    RELEASE_ASSERT(m_memory);
    auto& state = sharedState(*m_memory);
    if (!validState(state)) {
        WTFLogAlways("IPC::Semaphore: rejected corrupted shared state while signalling");
        return;
    }
    auto count = state.count.load(std::memory_order_relaxed);
    do {
        // The void API cannot report overflow. Reject it explicitly without
        // allowing externally writable shared data to crash the receiver.
        if (count == std::numeric_limits<uint64_t>::max()) {
            WTFLogAlways("IPC::Semaphore: rejected overflowing shared count");
            return;
        }
    } while (!state.count.compare_exchange_weak(count, count + 1, std::memory_order_release, std::memory_order_relaxed));
    wakeWaiter(state);
}

bool Semaphore::wait()
{
    return waitFor(Timeout::infinity());
}

bool Semaphore::waitFor(Timeout timeout)
{
    if (!m_memory)
        return false;
    auto& state = sharedState(*m_memory);
    for (;;) {
        if (!validState(state)) {
            WTFLogAlways("IPC::Semaphore: rejected corrupted shared state while waiting");
            return false;
        }
        if (tryAcquire(state))
            return true;
        if (!timeout.isInfinity() && timeout.didTimeOut())
            return false;
        auto deadline = wakeDeadline(timeout);
        int result = sem_clockwait(&state.wake, CLOCK_MONOTONIC, &deadline);
        if (!result)
            state.wakePending.store(0, std::memory_order_release);
        else if (errno != ETIMEDOUT && errno != EINTR)
            return false;
    }
}

void Semaphore::destroy()
{
    // Other processes can still own this object. Haiku's unnamed sem_destroy
    // owns no kernel resource; unmapping and closing our descriptors is enough.
    m_memory = nullptr;
}

} // namespace IPC
