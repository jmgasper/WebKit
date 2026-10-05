/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <OS.h>
#include <atomic>
#include <cstdint>

namespace WebKit {

// A page presented straight into the screen. A window with
// B_DIRECT_DEVICE_PIXELS learns where it is on the screen in frame buffer
// pixels and which parts of it are visible; the web process then copies each
// composited frame there with the GPU (Summit's Mesa patch 06) instead of
// reading it back for app_server to copy twice more. At 200 percent that read
// was about 10 ms of every frame.
//
// One page of memory per web view, made by the UI process and cloned by the
// web process. The UI process changes it holding `lock`, and waits there
// until the GPU has finished every copy already made, so nothing lands in a
// rectangle the window no longer owns; the web process presents holding the
// same lock.
struct DirectPresentStateHaiku {
    static constexpr uint32_t magicValue = 0x53445052; // 'SDPR'
    static constexpr uint32_t maximumRects = 200;

    uint32_t magic;
    sem_id lock;

    // Written by the UI process.
    uint32_t generation; // a new value whenever anything below changes
    uint32_t enabled;
    // The page's top left pixel, in the frame buffer.
    int32_t originX;
    int32_t originY;
    uint32_t frontBytesPerRow;
    // app_server's own copy of the screen, which the copies must keep current.
    area_id backArea;
    uint32_t backBytesPerRow;
    uint32_t backWidth;
    uint32_t backHeight;
    // The page's visible parts: left, top, right, bottom in frame buffer
    // pixels, both edges included.
    uint32_t rectCount;
    int32_t rects[maximumRects * 4];

    // Written by the web process.
    std::atomic<int32_t> inFlight; // copies the GPU has not finished
    std::atomic<uint32_t> presentedGeneration;
    std::atomic<uint32_t> presents;
};

static_assert(sizeof(DirectPresentStateHaiku) <= B_PAGE_SIZE);
static_assert(std::atomic<int32_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);

} // namespace WebKit
