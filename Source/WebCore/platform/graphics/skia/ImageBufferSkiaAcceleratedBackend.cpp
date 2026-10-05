/*
 * Copyright (C) 2024, 2025 Igalia S.L.
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
#include "ImageBufferSkiaAcceleratedBackend.h"

#if USE(SKIA)
#include "FontRenderOptions.h"
#include "GLContext.h"
#include "GLFence.h"
#include "GraphicsContextSkia.h"
#include "IntRect.h"
#include "NativeImage.h"
#include "PixelBuffer.h"
#include "PixelBufferConversion.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include "SkiaRecordingResult.h"
#include "SkiaUtilities.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPixmap.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/utils/SkNWayCanvas.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/TZoneMallocInlines.h>

#if PLATFORM(HAIKU)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

#if USE(COORDINATED_GRAPHICS)
#include "BitmapTexture.h"
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#include "CoordinatedPlatformLayerBufferRGB.h"
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#include "GraphicsLayerContentsDisplayDelegateCoordinated.h"
#include "TextureMapperFlags.h"
#endif

namespace WebCore {

// A canvas proxy that delegates all drawing operations to a single target canvas,
// which can be dynamically switched. This allows GraphicsContextSkia to hold a
// reference to this canvas while the actual target (recording vs surface) changes.
class SkiaSwitchableCanvas final : public SkNWayCanvas {
WTF_MAKE_TZONE_ALLOCATED(SkiaSwitchableCanvas);
public:
    explicit SkiaSwitchableCanvas(const IntSize& size)
        : SkNWayCanvas(size.width(), size.height())
    {
    }

    void switchToCanvas(SkCanvas* canvas)
    {
        SkNWayCanvas::removeAll();
        if (canvas)
            SkNWayCanvas::addCanvas(canvas);
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferSkiaAcceleratedBackend);

static inline bool shouldEnableDynamicMSAA()
{
    static std::once_flag onceFlag;
    static bool enableDynamicMSAA = false;
    std::call_once(onceFlag, [] {
        if (const char* enableDynamicMSAAEnv = getenv("WEBKIT_SKIA_ENABLE_DYNAMIC_MSAA")) {
            enableDynamicMSAA = *enableDynamicMSAAEnv != '0';
            return;
        }

#if PLATFORM(GTK)
        enableDynamicMSAA = true;
#else
        enableDynamicMSAA = false;
#endif
    });
    return enableDynamicMSAA;
}

std::unique_ptr<ImageBufferSkiaAcceleratedBackend> ImageBufferSkiaAcceleratedBackend::create(const ImageBufferParameters& parameters, const ImageBufferCreationContext& creationContext)
{
    IntSize backendSize = calculateSafeBackendSize(parameters);
    if (backendSize.isEmpty())
        return nullptr;

    // We always want to accelerate the canvas when Accelerated2DCanvas setting is true, even if skia CPU is enabled.
    if (parameters.purpose != RenderingPurpose::Canvas && !ProcessCapabilities::canUseAcceleratedBuffers())
        return nullptr;

#if !PLATFORM(WIN)
    if (!PlatformDisplay::sharedDisplayIfExists())
        return nullptr;
#endif

    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return nullptr;

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    RELEASE_ASSERT(grContext);

    auto imageInfo = SkImageInfo::Make(backendSize.width(), backendSize.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, parameters.colorSpace.platformColorSpace());
    auto msaaSampleCount = PlatformDisplay::sharedDisplay().msaaSampleCount();
    uint32_t flags = 0;
    if (parameters.purpose == RenderingPurpose::Canvas && msaaSampleCount && shouldEnableDynamicMSAA()) {
        flags |= SkSurfaceProps::kDynamicMSAA_Flag;
        msaaSampleCount = 1;
    }
    SkSurfaceProps properties = FontRenderOptions::singleton().createSurfaceProps(flags);
    auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kYes, imageInfo, msaaSampleCount, kTopLeft_GrSurfaceOrigin, &properties);
    if (!surface || !surface->getCanvas())
        return nullptr;
#if PLATFORM(HAIKU)
    // New GPU memory is not zeroed on NVK: a canvas that has not been drawn
    // yet showed what the memory held before (xbitlabs.com's FPS test showed
    // rows of an advertisement in its empty canvas). Start transparent.
    surface->getCanvas()->clear(SkColors::kTransparent);
#endif

    return create(parameters, creationContext, WTF::move(surface));
}

std::unique_ptr<ImageBufferSkiaAcceleratedBackend> ImageBufferSkiaAcceleratedBackend::create(const ImageBufferParameters& parameters, const ImageBufferCreationContext&, sk_sp<SkSurface>&& surface)
{
    ASSERT(surface);
    ASSERT(surface->getCanvas());
    return std::unique_ptr<ImageBufferSkiaAcceleratedBackend>(new ImageBufferSkiaAcceleratedBackend(parameters, WTF::move(surface)));
}

ImageBufferSkiaAcceleratedBackend::ImageBufferSkiaAcceleratedBackend(const ImageBufferParameters& parameters, sk_sp<SkSurface>&& surface)
    : ImageBufferSkiaSurfaceBackend(parameters, WTF::move(surface), RenderingMode::Accelerated)
{
#if USE(COORDINATED_GRAPHICS)
    // Use a content layer for canvas.
    if (parameters.purpose == RenderingPurpose::Canvas)
        m_layerContentsDisplayDelegate = GraphicsLayerContentsDisplayDelegateCoordinated::create();
#endif
}

ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()
{
    // Unwind the surface context's save/restore stack before destruction
    if (m_canvasRecordingContext)
        m_canvasRecordingContext->unwindStateStack();
}

GraphicsContext& ImageBufferSkiaAcceleratedBackend::context()
{
    if (purpose() != RenderingPurpose::Canvas)
        return ImageBufferSkiaSurfaceBackend::context();

    ensureCanvasRecordingContext();
    return *m_canvasRecordingContext;
}

GrDirectContext* ImageBufferSkiaAcceleratedBackend::grContext() const
{
    auto* recordingContext = m_surface->recordingContext();
    return recordingContext ? recordingContext->asDirectContext() : nullptr;
}

void ImageBufferSkiaAcceleratedBackend::ensureCanvasRecordingContext()
{
    if (m_canvasRecordingContext)
        return;

    // Create a switchable canvas that will delegate to either recording or surface canvas.
    // GraphicsContextSkia holds a reference to this canvas, which never changes - only the
    // target canvas it delegates to changes.
    m_switchableCanvas = makeUnique<SkiaSwitchableCanvas>(size());

    auto* recordingCanvas = m_pictureRecorder.beginRecording(size().width(), size().height());
    m_switchableCanvas->switchToCanvas(recordingCanvas);

    m_canvasRecordingContext = makeUnique<GraphicsContextSkia>(static_cast<SkCanvas&>(*m_switchableCanvas), RenderingMode::Accelerated, RenderingPurpose::Canvas);
    m_canvasRecordingContext->applyDeviceScaleFactor(resolutionScale());
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
    m_hasActiveRecording = true;
}

#if PLATFORM(HAIKU)
// SUMMIT_CANVAS_TRACE=1: canvas flushes and waits for the GPU that take over 20 ms.
static bool canvasTraceHaiku()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_CANVAS_TRACE");
        return value && !strcmp(value, "1");
    }();
    return enabled;
}

// Once a second with SUMMIT_CANVAS_TRACE=1: what the canvases' flushes did.
enum class CanvasEventHaiku { Flush, Replay, NoRecording, NoGrContext, NotCurrent, Prepare, PrepareNoDelegate, PrepareNoImage, Count };
static void countCanvasEventHaiku(CanvasEventHaiku event)
{
    if (!canvasTraceHaiku())
        return;
    static unsigned counts[static_cast<unsigned>(CanvasEventHaiku::Count)];
    static MonotonicTime since = MonotonicTime::now();
    ++counts[static_cast<unsigned>(event)];
    auto now = MonotonicTime::now();
    if (now - since < 1_s)
        return;
    fprintf(stderr, "Summit canvas: per second: flush %u, replay %u (none recorded %u, no GrContext %u, not current %u), prepare %u (no delegate %u, no image %u)\n",
        counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
    std::fill(std::begin(counts), std::end(counts), 0);
    since = now;
}
#define COUNT_CANVAS_EVENT_HAIKU(event) countCanvasEventHaiku(CanvasEventHaiku::event)
#else
#define COUNT_CANVAS_EVENT_HAIKU(event)
#endif

void ImageBufferSkiaAcceleratedBackend::replayCanvasRecordingContextIfNeeded()
{
    if (!m_canvasRecordingContext || !m_hasActiveRecording) {
        COUNT_CANVAS_EVENT_HAIKU(NoRecording);
        return;
    }

    auto* grContext = this->grContext();
    if (!grContext) {
        COUNT_CANVAS_EVENT_HAIKU(NoGrContext);
        return;
    }

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
        COUNT_CANVAS_EVENT_HAIKU(NotCurrent);
        return;
    }
    COUNT_CANVAS_EVENT_HAIKU(Replay);

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());

    IntRect recordRect(IntPoint(), size());
    auto recordingData = m_canvasRecordingContext->endRecording();
    auto picture = m_pictureRecorder.finishRecordingAsPicture();

    RefPtr<SkiaRecordingResult> recording = SkiaRecordingResult::create(WTF::move(picture), WTF::move(recordingData), recordRect, RenderingMode::Accelerated, false, 1.0);

    // Save the surface canvas save count before playback so we can undo any
    // unbalanced saves that the picture introduces.
    auto* surfaceCanvas = m_surface->getCanvas();
    auto surfaceSaveCount = surfaceCanvas->getSaveCount();

#if USE(TEXTURE_MAPPER)
    ASSERT(!recording->hasFences());
#endif
#if PLATFORM(HAIKU)
    const auto playbackStartedAt = canvasTraceHaiku() ? MonotonicTime::now() : MonotonicTime();
#endif
    recording->picture()->playback(surfaceCanvas);
#if PLATFORM(HAIKU)
    if (playbackStartedAt) {
        static unsigned replays;
        auto took = MonotonicTime::now() - playbackStartedAt;
        if (took > 20_ms || !(++replays % 120))
            fprintf(stderr, "Summit canvas: replay %.1f ms, %d operations, %dx%d\n", took.milliseconds(),
                recording->picture()->approximateOpCount(true), size().width(), size().height());
    }
#endif

    // Undo unbalanced saves from the picture playback on the surface canvas.
    surfaceCanvas->restoreToCount(surfaceSaveCount);

    // Switch the switchable canvas to target the surface canvas, then replay
    // state-replay state to bring the surface into the correct save/clip/CTM nesting.
    m_switchableCanvas->switchToCanvas(surfaceCanvas);
    m_canvasRecordingContext->replayStateOnCanvas(*surfaceCanvas);

    m_hasActiveRecording = false;
}

void ImageBufferSkiaAcceleratedBackend::flushContext()
{
    COUNT_CANVAS_EVENT_HAIKU(Flush);
#if PLATFORM(HAIKU)
    const auto startedAt = canvasTraceHaiku() ? MonotonicTime::now() : MonotonicTime();
#endif
    replayCanvasRecordingContextIfNeeded();
#if PLATFORM(HAIKU)
    const auto replayedAt = startedAt ? MonotonicTime::now() : MonotonicTime();
#endif

    auto* grContext = this->grContext();
    if (!grContext)
        return;

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());
    grContext->flushAndSubmit(m_surface.get(), GrSyncCpu::kNo);
#if PLATFORM(HAIKU)
    if (startedAt) {
        auto now = MonotonicTime::now();
        if ((now - startedAt) > 20_ms)
            fprintf(stderr, "Summit canvas: flush %.1f ms (replay %.1f, submit %.1f)\n", (now - startedAt).milliseconds(),
                (replayedAt - startedAt).milliseconds(), (now - replayedAt).milliseconds());
    }
#endif
}


void ImageBufferSkiaAcceleratedBackend::prepareForDisplay()
{
    COUNT_CANVAS_EVENT_HAIKU(Prepare);
#if USE(COORDINATED_GRAPHICS)
    if (!m_layerContentsDisplayDelegate) {
        COUNT_CANVAS_EVENT_HAIKU(PrepareNoDelegate);
        return;
    }

    auto image = createNativeImageReference();
    if (!image) {
        COUNT_CANVAS_EVENT_HAIKU(PrepareNoImage);
        return;
    }

    auto* grContext = this->grContext();
    if (!grContext)
        return;

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());

#if USE(TEXTURE_MAPPER)
    m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(),
        SkiaUtilities::flushAndSubmitSurfaceWithFence(grContext, m_surface.get())));
#else
    if (auto threadSafeGrContext = m_layerContentsDisplayDelegate->threadSafeGrContext())
        m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), threadSafeGrContext));
#endif

    // Re-enable recording mode for subsequent drawing operations.
    // This allows batching to occur again after each prepareForDisplay() cycle.
    if (m_canvasRecordingContext) {
        // Clean up state-replayed saves on the surface canvas before switching away.
        m_surface->getCanvas()->restoreToCount(1);

        auto* recordingCanvas = m_pictureRecorder.beginRecording(size().width(), size().height());
        m_switchableCanvas->switchToCanvas(recordingCanvas);

        // Replay state onto the new recording canvas to give it the exact same save/clip/CTM nesting.
        m_canvasRecordingContext->replayStateOnCanvas(*recordingCanvas);
        m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
        m_hasActiveRecording = true;
    }
#endif
}

RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::copyNativeImage()
{
    // SkSurface uses a copy-on-write mechanism for makeImageSnapshot(), so it's
    // always safe to return the SkImage without copying.
    return createNativeImageReference();
}

RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::createNativeImageReference()
{
    replayCanvasRecordingContextIfNeeded();

    auto* grContext = this->grContext();

    // If we're using MSAA, we need to flush the surface before calling makeImageSnapshot(),
    // because that call doesn't force the MSAA resolution, which can produce outdated results
    // in the resulting SkImage.
    auto& display = PlatformDisplay::sharedDisplay();
    if (grContext && display.msaaSampleCount() > 0 && display.skiaGLContext()->makeContextCurrent()) {
        ASSERT(grContext == display.skiaGrContext());
        grContext->flush(m_surface.get());
    }

    return NativeImage::create(m_surface->makeImageSnapshot(), grContext);
}

void ImageBufferSkiaAcceleratedBackend::getPixelBuffer(const IntRect& srcRect, PixelBuffer& destination)
{
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    // CPU needs to read pixels now, replay the recording.
    replayCanvasRecordingContextIfNeeded();

    ImageBufferSkiaSurfaceBackend::getPixelBuffer(srcRect, destination);
}

void ImageBufferSkiaAcceleratedBackend::putPixelBuffer(const PixelBufferSourceView& pixelBuffer, const IntRect& srcRect, const IntPoint& destPoint, AlphaPremultiplication destFormat)
{
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    // CPU needs to write pixels now, replay the recording.
    replayCanvasRecordingContextIfNeeded();

    ImageBufferSkiaSurfaceBackend::putPixelBuffer(pixelBuffer, srcRect, destPoint, destFormat);
}

#if USE(COORDINATED_GRAPHICS)
RefPtr<GraphicsLayerContentsDisplayDelegate> ImageBufferSkiaAcceleratedBackend::layerContentsDisplayDelegate() const
{
    return m_layerContentsDisplayDelegate;
}
#endif

} // namespace WebCore

#endif // USE(SKIA)
