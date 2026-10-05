/*
 * Copyright (C) 2017 Metrological Group B.V.
 * Copyright (C) 2017, 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)

#include "BitmapTexture.h"
#include "IntSize.h"
#include "PixelFormat.h"
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MallocSpan.h>
#include <wtf/Ref.h>
#include <wtf/ThreadSafeRefCounted.h>

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/private/chromium/GrDeferredDisplayList.h>
#include <skia/private/chromium/GrDeferredDisplayListRecorder.h>
#include <skia/private/chromium/GrSurfaceCharacterization.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebCore {
class BitmapTexture;
class GLFence;

class CoordinatedTileBuffer : public ThreadSafeRefCounted<CoordinatedTileBuffer> {
public:
    enum Flag {
        NoFlags = 0,
        SupportsAlpha = 1 << 0,
    };
    using Flags = unsigned;

    WEBCORE_EXPORT virtual ~CoordinatedTileBuffer();

    virtual IntSize size() const = 0;
    virtual PixelFormat pixelFormat() const = 0;
    virtual bool isBackedByOpenGL() const = 0;

    bool supportsAlpha() const { return m_flags & SupportsAlpha; }

    virtual void beginPainting();
    virtual void completePainting();
    void waitUntilPaintingComplete();

#if USE(SKIA)
    virtual SkCanvas* canvas() = 0;
#endif

    static void resetMemoryUsage();
    static double getMemoryUsage();

protected:
    explicit CoordinatedTileBuffer(Flags);

#if USE(SKIA)
    sk_sp<SkSurface> m_surface;
#endif

    static Lock s_layersMemoryUsageLock;
    static double s_currentLayersMemoryUsage;
    static double s_maxLayersMemoryUsage;

    enum class PaintingState {
        InProgress,
        Complete
    };

    struct {
        Lock lock;
        Condition condition;
        PaintingState state { PaintingState::Complete };
    } m_painting;

private:
    Flags m_flags;
};

class CoordinatedUnacceleratedTileBuffer final : public CoordinatedTileBuffer {
public:
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(const IntSize&, Flags);
#if PLATFORM(HAIKU)
    // For a painter that clears the buffer before it paints, on another
    // thread: the pixels are allocated by whoever asks for them first, and
    // not zeroed for the canvas. Zeroing 200 tiles of a megabyte each took
    // the main thread a quarter of a second in four while a page loaded.
    static Ref<CoordinatedTileBuffer> createForThreadedPainting(const IntSize&, Flags);
#endif
    WEBCORE_EXPORT virtual ~CoordinatedUnacceleratedTileBuffer();

    int stride() const { return m_size.width() * 4; }

#if PLATFORM(HAIKU)
    const unsigned char* data() const { ensureData(Zeroed::Yes); return m_data.span().data(); }
    unsigned char* data() { ensureData(Zeroed::Yes); return m_data.mutableSpan().data(); }

    // A layer over a page, a header that stays or a player's controls, is
    // mostly nothing: tiles whose every pixel is transparent. They were
    // uploaded, kept as textures and blended into every frame. The painter
    // says when a buffer is one, and the compositor keeps no texture for it.
    // SUMMIT_SKIP_EMPTY_TILES=0 treats them like any other.
    void noteIfTransparent();
    bool isTransparent() const { return m_isTransparent; }
#else
    const unsigned char* data() const { return m_data.span().data(); }
    unsigned char* data() { return m_data.mutableSpan().data(); }
#endif

private:
    CoordinatedUnacceleratedTileBuffer(const IntSize&, Flags);

    bool isBackedByOpenGL() const final { return false; }
    IntSize size() const final { return m_size; }
    PixelFormat pixelFormat() const final { return PixelFormat::BGRA8; }

#if USE(SKIA)
    SkCanvas* canvas() final;
#endif

#if PLATFORM(HAIKU)
    enum class Zeroed : bool { No, Yes };
    enum class Allocation : bool { Now, WhenAskedFor };
    CoordinatedUnacceleratedTileBuffer(const IntSize&, Flags, Allocation);
    void ensureData(Zeroed) const;

    mutable Lock m_dataLock;
    mutable MallocSpan<unsigned char> m_data;
    mutable std::atomic<bool> m_hasData { false };
    bool m_isTransparent { false };
#else
    MallocSpan<unsigned char> m_data;
#endif
    IntSize m_size;

    enum class PaintingState {
        InProgress,
        Complete
    };

    struct {
        Lock lock;
        Condition condition;
        PaintingState state { PaintingState::Complete };
    } m_painting;
};

#if USE(SKIA)
class CoordinatedAcceleratedTileBuffer final : public CoordinatedTileBuffer {
public:
#if USE(TEXTURE_MAPPER)
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(Ref<BitmapTexture>&&);
#else
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(const sk_sp<GrContextThreadSafeProxy>&, const IntSize&, Flags);
#endif
    WEBCORE_EXPORT virtual ~CoordinatedAcceleratedTileBuffer();

#if USE(TEXTURE_MAPPER)
    Ref<BitmapTexture> texture() const { return m_texture; }
    void serverWait();
#else
    sk_sp<GrDeferredDisplayList> displayList() const { return m_displayList; }
#endif

private:
#if USE(TEXTURE_MAPPER)
    CoordinatedAcceleratedTileBuffer(Ref<BitmapTexture>&&, Flags);
#else
    CoordinatedAcceleratedTileBuffer(GrSurfaceCharacterization&&, Flags);
#endif

    bool isBackedByOpenGL() const final { return true; }
    IntSize size() const final;
    PixelFormat pixelFormat() const final { return PixelFormat::RGBA8; }

    SkCanvas* canvas() final;
    void completePainting() final;

#if USE(TEXTURE_MAPPER)
    Ref<BitmapTexture> m_texture;
    std::unique_ptr<GLFence> m_fence;
#else
    GrSurfaceCharacterization m_characterization;
    std::optional<GrDeferredDisplayListRecorder> m_recorder;
    sk_sp<GrDeferredDisplayList> m_displayList;
#endif
};
#endif // USE(SKIA)

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
