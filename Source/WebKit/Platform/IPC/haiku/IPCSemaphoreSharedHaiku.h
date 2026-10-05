/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <atomic>
#include <cstdint>
#include <semaphore.h>

namespace IPC {

// The descriptor is a capability for one complete, versioned shared object.
// A separate 64-bit count avoids sem_post's signed 32-bit overflow in libroot.
// The POSIX semaphore carries at most one wake notification, never the count.
struct SharedSemaphoreHaiku {
    static constexpr uint64_t expectedMagic = 0x485749504353454dULL;
    uint64_t magic { expectedMagic };
    uint32_t version { 1 };
    uint32_t semaphoreSize { sizeof(sem_t) };
    std::atomic<uint64_t> count { 0 };
    std::atomic<uint32_t> wakePending { 0 };
    uint32_t reserved { 0 };
    sem_t wake { };
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

} // namespace IPC
