/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "DirectPresentHaiku.h"
#include <OS.h>
#include <Region.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace WebKit {

// The UI process end of DirectPresentStateHaiku, one per web view: where the
// view's page goes on the screen. See DirectPresentHaiku.h.
class DirectPresentTargetHaiku {
public:
    struct Target {
        bool enabled { false };
        int32_t originX { 0 };
        int32_t originY { 0 };
        uint32_t frontBytesPerRow { 0 };
        area_id backArea { -1 };
        uint32_t backBytesPerRow { 0 };
        uint32_t backWidth { 0 };
        uint32_t backHeight { 0 };
        std::vector<clipping_rect> rects;

        bool operator==(const Target& other) const
        {
            if (enabled != other.enabled)
                return false;
            if (!enabled)
                return true;
            if (originX != other.originX || originY != other.originY || frontBytesPerRow != other.frontBytesPerRow
                || backArea != other.backArea || backBytesPerRow != other.backBytesPerRow || backWidth != other.backWidth
                || backHeight != other.backHeight || rects.size() != other.rects.size())
                return false;
            for (size_t i = 0; i < rects.size(); ++i) {
                if (rects[i].left != other.rects[i].left || rects[i].top != other.rects[i].top
                    || rects[i].right != other.rects[i].right || rects[i].bottom != other.rects[i].bottom)
                    return false;
            }
            return true;
        }
    };

    static std::shared_ptr<DirectPresentTargetHaiku> create()
    {
        auto target = std::shared_ptr<DirectPresentTargetHaiku>(new DirectPresentTargetHaiku);
        return target->m_state ? target : nullptr;
    }

    ~DirectPresentTargetHaiku()
    {
        if (m_state)
            delete_sem(m_state->lock);
        if (m_area >= 0)
            delete_area(m_area);
    }

    area_id area() const { return m_area; }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Any thread; the callers take turns (WebKitView.cpp's direct lock).
    // Returns whether anything changed; a changed target needs the page
    // presented again in full. Returns once the GPU has finished every copy
    // made for the previous target, which a window that is being covered or
    // moved needs before app_server goes on, or at the deadline, whichever
    // comes first: app_server gives DirectConnected() half a second and then
    // kills the application, with its pages' copies still under way (that
    // hung the X399). Past the deadline an old copy can at worst land on what
    // now covers the window.
    bool update(const Target& target, bigtime_t deadline)
    {
        if (target == m_current)
            return false;
        bigtime_t lockWait = std::clamp<bigtime_t>(deadline - system_time(), 0, 30000);
        bool locked = acquire_sem_etc(m_state->lock, 1, B_RELATIVE_TIMEOUT, lockWait) == B_OK;
        m_state->enabled = target.enabled;
        m_state->originX = target.originX;
        m_state->originY = target.originY;
        m_state->frontBytesPerRow = target.frontBytesPerRow;
        m_state->backArea = target.backArea;
        m_state->backBytesPerRow = target.backBytesPerRow;
        m_state->backWidth = target.backWidth;
        m_state->backHeight = target.backHeight;
        uint32_t count = std::min<size_t>(target.rects.size(), DirectPresentStateHaiku::maximumRects);
        for (uint32_t i = 0; i < count; ++i) {
            m_state->rects[i * 4] = target.rects[i].left;
            m_state->rects[i * 4 + 1] = target.rects[i].top;
            m_state->rects[i * 4 + 2] = target.rects[i].right;
            m_state->rects[i * 4 + 3] = target.rects[i].bottom;
        }
        m_state->rectCount = count;
        m_state->generation++;
        // Holding the lock keeps new copies from starting while the old ones finish.
        deadline = std::min(deadline, system_time() + 60000);
        while (m_state->inFlight.load() > 0 && system_time() < deadline)
            snooze(100);
        if (locked)
            release_sem(m_state->lock);
        m_current = target;
        m_enabled = target.enabled;
        return true;
    }

    // Any thread. Something other than the page has drawn into its place on
    // the screen (app_server, from its own copy): the next frame goes there in
    // full rather than only what changed.
    void invalidate()
    {
        bool locked = acquire_sem_etc(m_state->lock, 1, B_RELATIVE_TIMEOUT, 30000) == B_OK;
        m_state->generation++;
        if (locked)
            release_sem(m_state->lock);
    }

private:
    DirectPresentTargetHaiku()
    {
        void* address = nullptr;
        m_area = create_area("direct present target", &address, B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK,
            B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
        if (m_area < 0)
            return;
        sem_id lock = create_sem(1, "direct present lock");
        if (lock < 0) {
            delete_area(m_area);
            m_area = -1;
            return;
        }
        std::memset(address, 0, B_PAGE_SIZE);
        m_state = new (address) DirectPresentStateHaiku;
        m_state->magic = DirectPresentStateHaiku::magicValue;
        m_state->lock = lock;
        m_state->backArea = -1;
    }

    area_id m_area { -1 };
    DirectPresentStateHaiku* m_state { nullptr };
    std::mutex m_mutex;
    Target m_current;
    std::atomic<bool> m_enabled { false };
};

} // namespace WebKit
