/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "BitmapFrameHaiku.h"
#include "PresentStatsHaiku.h"
#include <Bitmap.h>
#include <WebCore/ShareableBitmap.h>
#include <cstdlib>
#include <optional>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace WebKit {

// Constructing a BBitmap asks app_server for a shared area and registers it,
// which costs several times as much as copying a frame into one. Frames are
// recycled through this pool once the view that held the snapshot has let go.
class BitmapPoolHaiku : public std::enable_shared_from_this<BitmapPoolHaiku> {
public:
    // SUMMIT_PRESENT_POOL=0 keeps a fresh BBitmap per frame, the behaviour
    // before the pool existed, so its effect can be measured.
    static size_t maximumIdle()
    {
        static const size_t limit = [] {
            const char* value = std::getenv("SUMMIT_PRESENT_POOL");
            if (!value)
                return size_t(3);
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value, &end, 10);
            return (end != value && !*end && parsed <= 8) ? size_t(parsed) : size_t(3);
        }();
        return limit;
    }

    // Returns a bitmap of exactly this size that returns to the pool when the
    // last frame referencing it is released, or null if app_server refused it.
    std::shared_ptr<BBitmap> acquire(int32 width, int32 height)
    {
        std::unique_ptr<BBitmap> image;
        {
            std::lock_guard guard(m_mutex);
            for (size_t index = 0; index < m_idle.size(); ++index) {
                auto bounds = m_idle[index]->Bounds();
                if (bounds.IntegerWidth() == width - 1 && bounds.IntegerHeight() == height - 1) {
                    image = std::move(m_idle[index]);
                    m_idle.erase(m_idle.begin() + index);
                    break;
                }
            }
        }
        if (!image) {
            image = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), uint32_t(0), B_RGBA32);
            if (!image->IsValid())
                return nullptr;
        }
        return { image.release(), Recycler { weak_from_this() } };
    }

private:
    struct Recycler {
        std::weak_ptr<BitmapPoolHaiku> pool;
        void operator()(BBitmap* image) const
        {
            if (auto owner = pool.lock())
                owner->release(image);
            else
                delete image;
        }
    };

    // Called on whichever thread released the last reference to a frame.
    void release(BBitmap* image)
    {
        std::lock_guard guard(m_mutex);
        if (m_idle.size() >= maximumIdle()) {
            delete image;
            return;
        }
        m_idle.emplace_back(image);
    }

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<BBitmap>> m_idle;
};

// WebKit publishes on the application thread. A native window may retain its
// immutable snapshot while the next frame is published or the view is closed.
class BitmapPresenterHaiku {
public:
    struct Frame {
        uint64_t identifier;
        WebCore::IntSize viewSize;
        float scale;
        std::shared_ptr<const BBitmap> image;
    };

    bool publish(uint64_t identifier, const WebCore::ShareableBitmap& bitmap, const WebCore::IntSize& viewSize, float scale)
    {
        auto expected = bitmapFrameSizeHaiku(viewSize, scale);
        // At a fractional scale the compositor truncates where this rounds up,
        // so a device size one pixel short of the expected one is still whole.
        if (!identifier || !expected || std::abs(bitmap.size().width() - expected->width()) > 1
            || std::abs(bitmap.size().height() - expected->height()) > 1)
            return false;
        auto size = std::optional { bitmap.size() };
        if (bitmap.bytesPerRow() != static_cast<size_t>(size->width()) * 4
            || bitmap.span().size() != bitmap.bytesPerRow() * size->height())
            return false;
        {
            std::lock_guard guard(m_mutex);
            if (m_closed || identifier <= m_lastIdentifier)
                return false;
        }
        // Do not expose the WebContent mapping to app_server or retain mutable
        // shared pixels after acknowledging presentation to the sender.
        auto image = m_pool->acquire(size->width(), size->height());
        if (!image || image->BytesPerRow() != static_cast<int32>(bitmap.bytesPerRow())
            || static_cast<size_t>(image->BitsLength()) != bitmap.span().size())
            return false;
        bigtime_t copyStart = PresentStageStatsHaiku::enabled() ? system_time() : 0;
        std::memcpy(image->Bits(), bitmap.span().data(), bitmap.span().size());
        if (copyStart) {
            static PresentStageStatsHaiku copy("copy (ui)");
            copy.add(system_time() - copyStart, size->width() * double(size->height()) / 1e6);
        }
        auto frame = std::make_shared<const Frame>(Frame { identifier, viewSize, scale, std::move(image) });
        std::shared_ptr<const Frame> previous;
        {
            std::lock_guard guard(m_mutex);
            if (m_closed || identifier <= m_lastIdentifier)
                return false;
            m_lastIdentifier = identifier;
            previous = std::exchange(m_frame, std::move(frame));
        }
        return true;
    }

    // A frame drawn in place from memory the caller shares; the image's
    // deleter says when no view shows it any more.
    bool publishShared(uint64_t identifier, std::shared_ptr<const BBitmap>&& image, const WebCore::IntSize& viewSize, float scale)
    {
        auto expected = bitmapFrameSizeHaiku(viewSize, scale);
        if (!identifier || !expected || !image)
            return false;
        auto bounds = image->Bounds();
        if (std::abs(bounds.IntegerWidth() + 1 - expected->width()) > 1 || std::abs(bounds.IntegerHeight() + 1 - expected->height()) > 1)
            return false;
        auto frame = std::make_shared<const Frame>(Frame { identifier, viewSize, scale, std::move(image) });
        std::shared_ptr<const Frame> previous;
        {
            std::lock_guard guard(m_mutex);
            if (m_closed || identifier <= m_lastIdentifier)
                return false;
            m_lastIdentifier = identifier;
            previous = std::exchange(m_frame, std::move(frame));
        }
        return true;
    }

    std::shared_ptr<const Frame> snapshot() const
    {
        std::lock_guard guard(m_mutex);
        return m_frame;
    }

    void close()
    {
        std::shared_ptr<const Frame> previous;
        {
            std::lock_guard guard(m_mutex);
            m_closed = true;
            previous = std::exchange(m_frame, nullptr);
        }
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const Frame> m_frame;
    std::shared_ptr<BitmapPoolHaiku> m_pool { std::make_shared<BitmapPoolHaiku>() };
    uint64_t m_lastIdentifier { 0 };
    bool m_closed { false };
};

} // namespace WebKit
