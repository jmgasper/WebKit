/*
 * Copyright (C) 2014, 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadedCompositor.h"

#if PLATFORM(HAIKU)
#include "StartupTraceHaiku.h"
#endif

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedSurface.h"
#include "CoordinatedSceneState.h"
#include "LayerTreeHost.h"
#include "RenderProcessInfo.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/CoordinatedBackingStore.h>
#include <WebCore/CoordinatedBackingStoreTile.h>
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/Damage.h>
#include <WebCore/FontCache.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/Settings.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/SkiaDamageRegion.h>
#include <WebCore/TransformationMatrix.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkPaint.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringToIntegerConversion.h>

#if PLATFORM(HAIKU)
#include <wtf/haiku/RunLoopHaiku.h>
#include <OS.h>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#endif

#if USE(GLIB_EVENT_LOOP)
#include <wtf/glib/RunLoopSourcePriority.h>
#endif

#if USE(TEXTURE_MAPPER)
#include <WebCore/TextureMapperLayer.h>
#endif

#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif

#if PLATFORM(HAIKU)
namespace WebCore {
unsigned summitTakeTileHolesHaiku(unsigned& textureless);
}
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadedCompositor);

#if PLATFORM(HAIKU)
static bool traceCompositorTiming()
{
    static bool enabled = [] {
        const char* value = std::getenv("SUMMIT_COMPOSITOR_TIMING_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}
#endif

Ref<ThreadedCompositor> ThreadedCompositor::create(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
{
    return adoptRef(*new ThreadedCompositor(webPage, layerTreeHost, sceneState));
}

ThreadedCompositor::ThreadedCompositor(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
#if PLATFORM(HAIKU)
    // Ahead of page scripts and loading, like the scrolling thread it serves.
    : m_workQueue(WorkQueue::create("org.webkit.ThreadedCompositor"_s, WorkQueue::QOS::UserInteractive))
#else
    : m_workQueue(WorkQueue::create("org.webkit.ThreadedCompositor"_s))
#endif
    , m_layerTreeHost(&layerTreeHost)
    , m_surface(AcceleratedSurface::create(webPage, [this] { frameComplete(); }, AcceleratedSurface::RenderingPurpose::Composited))
    , m_sceneState(&sceneState)
#if USE(TEXTURE_MAPPER)
    , m_flipY(!m_surface->shouldPaintMirrored())
#endif
    , m_renderTimer(m_workQueue->runLoop(), "ThreadedCompositor::RenderTimer"_s, this, &ThreadedCompositor::renderLayerTree)
{
    ASSERT(RunLoop::isMain());

#if PLATFORM(HAIKU)
    unsigned framesPerSecond = webPage.corePage()->displayNominalFramesPerSecond().value_or(60);
    if (const char* value = getenv("SUMMIT_COMPOSITOR_MAX_FPS")) {
        if (auto override = parseInteger<unsigned>(StringView::fromLatin1(value)); override && (!*override || (*override >= 30 && *override <= 240)))
            framesPerSecond = *override;
    }
    if (framesPerSecond)
        m_minimumFrameInterval = 1_s / framesPerSecond;
    m_surface->setRenderTargetReleasedHandler([this] { renderTargetReleased(); });
    m_surface->setRedisplayHandlerHaiku([this] { requestComposition(CompositionReason::Animation); });
#endif

    m_didCompositeRunLoopObserver = makeUnique<RunLoopObserver>(RunLoopObserver::WellKnownOrder::GraphicsCommit, [this] {
        this->didCompositeRunLoopObserverFired();
    });

    initializeFPSCounter();
#if ENABLE(DAMAGE_TRACKING)
#if USE(TEXTURE_MAPPER)
    m_damage.visualizer = TextureMapperDamageVisualizer::create();
#else
    // The margin TextureMapperDamageVisualizer takes means nothing here, since the overlay fills the
    // damage as a region, but the same variable enables both so it does not depend on the compositor.
    if (const auto* showDamageEnvvar = getenv("WEBKIT_SHOW_DAMAGE")) {
        if (auto value = parseInteger<unsigned>(StringView::fromLatin1(showDamageEnvvar)); value && *value)
            m_damage.showAccumulatedDamageOverlay = true;
    }
#endif
#endif

    updateSceneAttributes(webPage.size(), webPage.deviceScaleFactor());

    m_surface->didCreateCompositingRunLoop(m_workQueue->runLoop());

    m_workQueue->dispatchSync([this] {
        // GLNativeWindowType depends on the EGL implementation: reinterpret_cast works
        // for pointers (only if they are 64-bit wide and not for other cases), and static_cast for
        // numeric types (and when needed they get extended to 64-bit) but not for pointers. Using
        // a plain C cast expression in this one instance works in all cases.
        static_assert(sizeof(GLNativeWindowType) <= sizeof(uint64_t), "GLNativeWindowType must not be longer than 64 bits.");
        auto nativeSurfaceHandle = (GLNativeWindowType)m_surface->window();
        auto context = GLContext::create(PlatformDisplay::sharedDisplay(), nativeSurfaceHandle);
        if (!context || !context->makeContextCurrent()) {
            Locker locker { m_state.lock };
            m_state.state = State::Invalidated;
            return;
        }

        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
#if PLATFORM(HAIKU)
        SUMMIT_STARTUP_TRACE("compositor GL context ready");
#endif

#if USE(TEXTURE_MAPPER)
        m_context = WTF::move(context);
        m_textureMapper = TextureMapper::create();
#else
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(WTF::move(context));
        m_threadSafeGrContext = PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy();
#endif
    });

#if PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)
    // The two programs of every first frame, before it is asked for.
    if (!getenv("SUMMIT_WARM_SHADERS") || strcmp(getenv("SUMMIT_WARM_SHADERS"), "0")) {
        m_workQueue->dispatch([this, protectedThis = Ref { *this }] {
            warmShaderProgramsHaiku(2, false);
        });
    }
#endif

}

#if PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)
// Shader programs the page is likely to need soon are compiled one per task
// (TextureMapper::warmShaderProgramsHaiku()): the two every first frame draws
// with while the page is fetched, the others once a frame has shown the
// page's tiles, each when no composition is waiting and none was sent for
// 100 ms, so neither the first frames nor a composition asked for meanwhile
// wait for them. On the Raspberry Pi 4, without a shader cache, the warm-up
// of all of them before the first frame put it 2.2 s later, and compiles
// right after a blank first frame held back the one with the page.
// SUMMIT_WARM_SHADERS=0 compiles each program when it is first drawn with.
void ThreadedCompositor::warmShaderProgramsHaiku(unsigned remaining, bool waitsForQuiet)
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    if (!remaining || !m_context || !m_textureMapper)
        return;
    if (waitsForQuiet) {
        static constexpr Seconds quiet = 100_ms;
        Seconds wait;
        {
            Locker locker { m_state.lock };
            if (m_state.state == State::Invalidated)
                return;
            auto sinceFrame = MonotonicTime::now() - m_lastFrameSentHaiku;
            if (m_state.state != State::Idle || m_state.isRenderTimerActive)
                wait = quiet;
            else if (sinceFrame < quiet)
                wait = quiet - sinceFrame;
        }
        if (wait > 0_s) {
            m_workQueue->dispatchAfter(wait, [this, protectedThis = Ref { *this }, remaining] {
                warmShaderProgramsHaiku(remaining, true);
            });
            return;
        }
    }
    if (!m_context->makeContextCurrent())
        return;
    m_textureMapper->warmShaderProgramsHaiku();
    m_workQueue->dispatch([this, protectedThis = Ref { *this }, remaining, waitsForQuiet] {
        warmShaderProgramsHaiku(remaining - 1, waitsForQuiet);
    });
}
#endif

ThreadedCompositor::~ThreadedCompositor() = default;

uint64_t ThreadedCompositor::surfaceID() const
{
    ASSERT(RunLoop::isMain());
    return m_surface->surfaceID();
}

void ThreadedCompositor::invalidate()
{
    ASSERT(RunLoop::isMain());

    {
        Locker locker { m_state.lock };
        stopRenderTimer();
        m_state.didCompositeRenderingUpdateFunction = nullptr;
        m_state.state = State::Invalidated;
    }

    m_didCompositeRunLoopObserver->invalidate();
    m_workQueue->dispatchSync([this] {
#if USE(TEXTURE_MAPPER)
        if (!m_context || !m_context->makeContextCurrent())
            return;
#endif

        // Update the scene at this point ensures the layers state are correctly propagated.
        flushCompositingState(CompositionReason::RenderingUpdate);

        m_sceneState->invalidateCommittedLayers();
#if USE(TEXTURE_MAPPER)
        m_textureMapper = nullptr;
#endif
        m_surface->willDestroyGLContext();
#if USE(TEXTURE_MAPPER)
        m_context = nullptr;
#else
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
#endif
    });
    m_sceneState = nullptr;
    m_layerTreeHost = nullptr;
    m_surface->willDestroyCompositingRunLoop();
    m_surface = nullptr;
}

void ThreadedCompositor::startRenderTimer()
{
    assertIsHeld(m_state.lock);
    ASSERT(!m_state.isRenderTimerActive);
    m_state.isRenderTimerActive = true;
    updateRenderTimer();
}

void ThreadedCompositor::stopRenderTimer()
{
    assertIsHeld(m_state.lock);
    m_state.isRenderTimerActive = false;
    updateRenderTimer();
}

void ThreadedCompositor::updateRenderTimer()
{
    assertIsHeld(m_state.lock);

    // m_renderTimer is bound to the compositor thread's run loop (m_workQueue), so it must be started
    // and stopped there. The render timer's desired state is tracked synchronously under m_state.lock by
    // m_state.isRenderTimerActive and may be changed from the main thread (e.g. from suspend()/resume()/
    // invalidate()), so hop to the compositor thread to bring the timer in line with that state.
    if (!m_workQueue->runLoop().isCurrent()) {
        m_workQueue->dispatch([protectedThis = Ref { *this }] {
            Locker locker { protectedThis->m_state.lock };
            protectedThis->updateRenderTimer();
        });
        return;
    }

    if (m_state.isRenderTimerActive) {
        if (!m_renderTimer.isActive()) {
            Seconds delay;
#if PLATFORM(HAIKU)
            if (testStallHaiku(TestStallHaiku::Timer))
                return;
            if (testStallHaiku(TestStallHaiku::Late)) {
                m_renderTimer.startOneShot(1_h);
                return;
            }
            if (m_minimumFrameInterval && m_lastRenderTimestamp) {
                Seconds elapsed = MonotonicTime::now() - m_lastRenderTimestamp;
                if (elapsed < m_minimumFrameInterval)
                    delay = m_minimumFrameInterval - elapsed;
            }
#endif
            m_renderTimer.startOneShot(delay);
        }
    } else
        m_renderTimer.stop();
}

bool ThreadedCompositor::isOnlyRenderingUpdatePendingAndWaitingForTiles() const
{
    assertIsHeld(m_state.lock);
    return m_state.reasons.containsOnly({ CompositionReason::RenderingUpdate }) && m_state.isWaitingForTiles;
}

void ThreadedCompositor::suspend()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(false);

    if (++m_suspendedCount > 1)
        return;

    Locker locker { m_state.lock };
    stopRenderTimer();
}

void ThreadedCompositor::resume()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(true);

    ASSERT(m_suspendedCount > 0);
    if (--m_suspendedCount > 0)
        return;

    Locker locker { m_state.lock };
    if (m_state.state == State::Scheduled && !isOnlyRenderingUpdatePendingAndWaitingForTiles())
        startRenderTimer();
}

bool ThreadedCompositor::isActive() const
{
    Locker locker { m_state.lock };
    return m_state.state != State::Idle && m_state.state != State::Invalidated;
}

void ThreadedCompositor::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->backgroundColorDidChange();
}

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void ThreadedCompositor::preferredBufferFormatsDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->preferredBufferFormatsDidChange();
}
#endif

void ThreadedCompositor::pendingTilesDidChange()
{
    Locker locker { m_state.lock };
    if (!m_state.isWaitingForTiles)
        return;

    if (m_sceneState->pendingTiles())
        return;

    m_state.isWaitingForTiles = false;
    scheduleUpdateLocked();
}

void ThreadedCompositor::setSize(const IntSize& size, float deviceScaleFactor)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_attributes.lock };
    updateSceneAttributes(size, deviceScaleFactor);
}

#if ENABLE(DAMAGE_TRACKING)
void ThreadedCompositor::setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>> flags, unsigned rectangleThreshold)
{
    m_damage.flags = flags;

    rectangleThreshold = Damage::clampRectangleThreshold(rectangleThreshold);
    if (m_surface) {
        m_surface->setFrameDamageRectangleThreshold(rectangleThreshold);
        m_surface->setDamageUsedForCompositing(damageUsedForCompositing());
    }
}

bool ThreadedCompositor::damageUsedForCompositing() const
{
    // Only the Skia compositor draws just the damage rects. TextureMapper scissors with the damage
    // bounds, which needs neither fine-grained records nor swap-chain accumulation.
#if USE(TEXTURE_MAPPER)
    return false;
#else
    return m_damage.flags && m_damage.flags->contains(DamagePropagationFlags::UseForCompositing);
#endif
}

bool ThreadedCompositor::drawsOverlay() const
{
    // The damage overlay is drawn by paintToSkiaCanvas(), the visualizer by paintToTextureMapper(), so
    // each path only has its own. Both draw over the damage itself, and damaging what they drew would
    // feed their own rects back into what they show next, growing until they cover the surface. So the
    // frame repaints in full instead. The FPS counter has no such loop and damages its own box.
#if USE(TEXTURE_MAPPER)
    return !!m_damage.visualizer;
#else
    return m_damage.showAccumulatedDamageOverlay;
#endif
}

void ThreadedCompositor::enableFrameDamageNotificationForTesting()
{
    m_damage.shouldNotifyFrameDamageForTesting = true;
}
#endif

void ThreadedCompositor::flushCompositingState(const OptionSet<CompositionReason>& reasons)
{
    if (reasons.hasExactlyOneBitSet() && reasons.contains(CompositionReason::Animation))
        return;

#if ASSERT_ENABLED
    {
        Locker locker { m_state.lock };
        ASSERT(!reasons.contains(CompositionReason::RenderingUpdate) || !m_state.isWaitingForTiles);
    }
#endif

    m_sceneState->flushCompositingState(reasons);
}

TargetContents ThreadedCompositor::paintToCurrentGLContext(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
#if USE(TEXTURE_MAPPER)
    paintToTextureMapper(matrix, size, reasons);
    return TargetContents::Valid;
#else
    return paintToSkiaCanvas(matrix, size, reasons);
#endif
}

#if USE(TEXTURE_MAPPER)
void ThreadedCompositor::paintToTextureMapper(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
#if PLATFORM(HAIKU)
    const bool tracePaint = traceCompositorTiming();
    const bigtime_t paintStartedAt = tracePaint ? system_time() : 0;
#endif
    FloatRect clipRect(FloatPoint { }, size);
    TextureMapperLayer& currentRootLayer = m_sceneState->rootLayer().ensureTarget();
    if (currentRootLayer.transform() != matrix)
        currentRootLayer.setTransform(matrix);

    bool sceneHasRunningAnimations = currentRootLayer.applyAnimationsRecursively(MonotonicTime::now());

    m_textureMapper->beginPainting(m_flipY ? TextureMapper::FlipY::Yes : TextureMapper::FlipY::No);
    m_textureMapper->beginClip(TransformationMatrix(), FloatRoundedRect(clipRect));
#if PLATFORM(HAIKU)
    const bigtime_t clipStartedAt = tracePaint ? system_time() : 0;
#endif

#if ENABLE(DAMAGE_TRACKING)
    std::optional<FloatRoundedRect> rectContainingRegionThatActuallyChanged;
    currentRootLayer.prepareForPainting(*m_textureMapper);
    if (m_damage.flags) {
        Damage frameDamage(size, m_damage.flags->contains(DamagePropagationFlags::Unified) ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);

        WTFBeginSignpost(this, CollectDamage);
        currentRootLayer.collectDamage(*m_textureMapper, frameDamage);
        WTFEndSignpost(this, CollectDamage);
#if PLATFORM(HAIKU)
        if (CoordinatedBackingStore::takeShowsLateUploadHaiku())
            frameDamage = Damage(size, Damage::Mode::Full);
#endif

        recordFrameDamage(WTF::move(frameDamage));

        if (m_damage.flags->contains(DamagePropagationFlags::UseForCompositing)) {
            if (drawsOverlay()) {
                // No damage means repaint the whole frame, so skip the scissor below too.
                m_textureMapper->setDamage(std::nullopt);
            } else {
                const auto& damageSinceLastSurfaceUse = m_surface->renderTargetDamage();
                if (damageSinceLastSurfaceUse && !FloatRect(damageSinceLastSurfaceUse->bounds()).contains(clipRect))
                    rectContainingRegionThatActuallyChanged = FloatRoundedRect(damageSinceLastSurfaceUse->bounds());

                m_textureMapper->setDamage(damageSinceLastSurfaceUse);
            }
        }
    }

    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->beginClip(TransformationMatrix(), *rectContainingRegionThatActuallyChanged);
#endif

#if PLATFORM(HAIKU)
    const bigtime_t clearedAt = tracePaint ? system_time() : 0;
#endif
    m_surface->clear(reasons);
#if PLATFORM(HAIKU)
    const bigtime_t layerPaintStartedAt = tracePaint ? system_time() : 0;
#endif

    WTFBeginSignpost(this, PaintTextureMapperLayerTree);
    currentRootLayer.paint(*m_textureMapper);
    WTFEndSignpost(this, PaintTextureMapperLayerTree);
#if PLATFORM(HAIKU)
    const bigtime_t layerPaintedAt = tracePaint ? system_time() : 0;
#endif

#if ENABLE(DAMAGE_TRACKING)
    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->endClip();
#endif

#if ENABLE(DAMAGE_TRACKING)
    if (m_damage.visualizer) {
        m_damage.visualizer->paintDamage(*m_textureMapper, m_surface->frameDamage());
        // When damage visualizer is active, we cannot send the original damage to the platform as in this case
        // the damage rects visualized previous frame may not get erased if platform actually uses damage.
        m_surface->setFrameDamageForPlatformOnly(Damage(size, Damage::Mode::Full));
    }
#endif

    m_textureMapper->endClip();
    m_textureMapper->endPainting();
#if PLATFORM(HAIKU)
    if (tracePaint) {
        const bigtime_t finishedAt = system_time();
        if (finishedAt - paintStartedAt >= 30000)
            std::fprintf(stderr, "Summit compositor paint: total=%.1f setup=%.1f damage=%.1f clear=%.1f layers=%.1f finish=%.1f\n",
                (finishedAt - paintStartedAt) / 1000.0, (clipStartedAt - paintStartedAt) / 1000.0,
                (clearedAt - clipStartedAt) / 1000.0, (layerPaintStartedAt - clearedAt) / 1000.0,
                (layerPaintedAt - layerPaintStartedAt) / 1000.0, (finishedAt - layerPaintedAt) / 1000.0);
    }
#endif

    if (sceneHasRunningAnimations)
        requestComposition(CompositionReason::Animation);
}

#else

#if ENABLE(DAMAGE_TRACKING)
static void drawDamageOverlay(SkCanvas& canvas, const Damage& damage, const IntSize& size, SkColor color)
{
    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(color);

    // Drawn as a region, so the translucent overlay composites once even where the rects touch. No region
    // means the damage covers the whole surface, which the overlay has to show as such.
    if (auto damageRegion = SkiaDamageRegion::create(damage, size))
        damageRegion->fillCanvasInDeviceSpace(canvas, paint);
    else
        canvas.drawPaint(paint);
}
#endif

TargetContents ThreadedCompositor::paintToSkiaCanvas(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
    auto* canvas = m_surface->canvas();
    if (!canvas)
        return TargetContents::Invalid;

    auto& rootLayer = m_sceneState->rootLayer().ensureTarget();
    rootLayer.setTransform(matrix);

    // paint() collects this frame's damage into frameDamage, and limits the draw to what this target must
    // redraw: priorTargetDamage - what changed since the target was last current, read before this frame
    // is recorded - folded with frameDamage. An unset priorTargetDamage repaints the whole target.
    std::optional<Damage> frameDamage;
    std::optional<Damage> priorTargetDamage;
    const std::optional<SkColor> clearColor = m_surface->skiaClearColor(reasons);

#if ENABLE(DAMAGE_TRACKING)
    // Also collect for the accumulated-damage overlay, so it works with the compositing feature off.
    if (m_damage.flags || m_damage.showAccumulatedDamageOverlay)
        frameDamage = Damage(size, m_damage.flags && m_damage.flags->contains(DamagePropagationFlags::Unified) ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);

    // No layer damages the counter's box, so seed it here, before paint() collects.
    if (frameDamage && m_fpsCounter.drawsFPS)
        frameDamage->add(takeFPSCounterDamage());

    // Read the target's record before this frame is recorded into it.
    if (damageUsedForCompositing() && !drawsOverlay())
        priorTargetDamage = m_surface->renderTargetDamage();
#endif

    canvas->save();
    const bool hasRunningAnimations = rootLayer.paint(*canvas, frameDamage, priorTargetDamage, clearColor);
    canvas->restore();

#if ENABLE(DAMAGE_TRACKING)
    // Record into every target, so each one repaints this frame's damage the next time it is used.
    if (frameDamage)
        recordFrameDamage(WTF::move(*frameDamage));

    if (m_damage.showAccumulatedDamageOverlay) {
        if (const auto& damage = m_surface->renderTargetDamage(); damage && !damage->isEmpty()) {
            drawDamageOverlay(*canvas, *damage, size, SkColorSetARGB(128, 0, 255, 0));

            // Schedule one more composition, so that once damage stops the next frame repaints the
            // overlay away instead of leaving it frozen on an idle page.
            requestComposition(CompositionReason::Animation);
        }
    }

    // An overlay is redrawn every frame and is in no layer's damage, so tell the platform the whole
    // surface changed. Otherwise it may leave the previous frame's overlay on screen.
    if (drawsOverlay())
        m_surface->setFrameDamageForPlatformOnly(Damage(size, Damage::Mode::Full));
#endif

    if (m_fpsCounter.drawsFPS)
        drawFPSCounter(*canvas);

    if (auto* surface = canvas->getSurface())
        PlatformDisplay::sharedDisplay().skiaGrContext()->flushAndSubmit(surface, GrSyncCpu::kNo);

    if (hasRunningAnimations)
        requestComposition(CompositionReason::Animation);

    return TargetContents::Valid;
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void ThreadedCompositor::recordFrameDamage(Damage&& damage)
{
    if (m_damage.shouldNotifyFrameDamageForTesting && m_layerTreeHost)
        m_layerTreeHost->notifyFrameDamageForTesting(damage.regionForTesting());

    // Nothing changed this frame: an empty damage contributes nothing to the targets, since Damage::add()
    // ignores empty damage. Skip it rather than replace the recorded frame damage with an empty one.
    if (damage.isEmpty())
        return;

    m_surface->setFrameDamage(WTF::move(damage));
}
#endif

#if HAVE(OS_SIGNPOST) || USE(SYSPROF_CAPTURE)
static String reasonsToString(const OptionSet<CompositionReason>& reasons)
{
    StringBuilder builder;
    for (auto reason : reasons) {
        if (!builder.isEmpty())
            builder.append(", "_s);
        builder.append(enumName(reason));
    }
    return builder.toString();
}
#endif

void ThreadedCompositor::renderLayerTree()
{
    ASSERT(m_sceneState);
    ASSERT(m_workQueue->runLoop().isCurrent());
#if PLATFORM(GTK) || PLATFORM(WPE)
    TraceScope traceScope(RenderLayerTreeStart, RenderLayerTreeEnd);
#endif

    if (m_suspendedCount > 0)
        return;

#if PLATFORM(HAIKU)
    // Every buffer of the swap chain is still with the UI process: the view
    // shows one and the rest are on their way back. Painting now would make the
    // chain allocate another full-size buffer (27 MB at 200%), and keep it,
    // since spare buffers are freed only once the page is hidden. Keep the
    // request and wait; renderTargetReleased() runs it as soon as one returns.
    // A buffer that never comes back must not freeze the page, so after a
    // while it paints anyway and the chain grows by one.
    // SUMMIT_SWAPCHAIN_WAIT=0 paints into a new buffer at once instead.
    static const bool waitForRenderTarget = [] {
        const char* value = getenv("SUMMIT_SWAPCHAIN_WAIT");
        return !value || strcmp(value, "0");
    }();
    if (waitForRenderTarget) {
        static constexpr Seconds waitLimit = 2_s;
        IntSize size;
        {
            Locker locker { m_attributes.lock };
            size = m_attributes.viewportSize;
        }
        Locker locker { m_state.lock };
        // Only a live, scheduled composition waits; the checks below end the others.
        bool live = m_state.state != State::Invalidated && m_state.isRenderTimerActive;
        if (live && !size.isEmpty() && !m_surface->hasRenderTargetFor(size)) {
            auto now = MonotonicTime::now();
            if (!m_isWaitingForRenderTarget) {
                m_isWaitingForRenderTarget = true;
                m_renderTargetWaitStartedAt = now;
            }
            Seconds waited = now - m_renderTargetWaitStartedAt;
            if (waited < waitLimit) {
                m_renderTimer.startOneShot(waitLimit - waited);
                return;
            }
            WTFLogAlways("Summit compositor: no buffer came back for %.1f s; painting into a new one", waited.seconds());
        }
        m_isWaitingForRenderTarget = false;
    }
#endif

    OptionSet<CompositionReason> reasons;
    bool shouldNotifiyDidComposite = false;
    {
        Locker locker { m_state.lock };

        if (m_state.state == State::Invalidated)
            return;

        // The timer has been stopped.
        if (!m_state.isRenderTimerActive)
            return;

        m_state.isRenderTimerActive = false;
        reasons = std::exchange(m_state.reasons, { });
        if (reasons.contains(CompositionReason::RenderingUpdate)) {
            if (m_state.isWaitingForTiles) {
                reasons.remove(CompositionReason::RenderingUpdate);
                m_state.reasons.add(CompositionReason::RenderingUpdate);
            } else
                shouldNotifiyDidComposite = !!m_state.didCompositeRenderingUpdateFunction;
        }

        ASSERT(m_state.state == State::Scheduled);
        m_state.state = State::InProgress;
#if PLATFORM(HAIKU)
        m_lastRenderTimestamp = MonotonicTime::now();
        m_inProgressSinceHaiku = m_lastRenderTimestamp;
        ++m_renderCountHaiku;
#endif
    }

#if PLATFORM(HAIKU)
    const bool traceTiming = traceCompositorTiming();
    const bigtime_t startedAt = traceTiming ? system_time() : 0;
#endif

#if USE(TEXTURE_MAPPER)
    if (!m_context || !m_context->makeContextCurrent())
        return;
#endif

    // Retrieve the scene attributes in a thread-safe manner.
    IntSize viewportSize;
    float deviceScaleFactor;
    {
        Locker locker { m_attributes.lock };
        viewportSize = m_attributes.viewportSize;
        deviceScaleFactor = m_attributes.deviceScaleFactor;
    }

    if (viewportSize.isEmpty())
        return;


    TransformationMatrix viewportTransform;
    viewportTransform.scale(deviceScaleFactor);

    m_surface->willRenderFrame(viewportSize);
#if PLATFORM(HAIKU)
    const bigtime_t surfaceAt = traceTiming ? system_time() : 0;
#endif

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->willRenderFrame();
    });

#if PLATFORM(HAIKU)
    if (traceTiming) {
        CoordinatedBackingStoreTile::setCountsUpdatesHaiku(true);
        TextureMapper::setCountsDrawsHaiku(true);
    }
#if USE(TEXTURE_MAPPER)
    // Until the shader warm-up starts, the tile counts say when a frame first
    // shows the page's tiles.
    else
        CoordinatedBackingStoreTile::setCountsUpdatesHaiku(!m_shaderWarmupStartedHaiku);
#endif
    // SUMMIT_TILE_UPLOAD_BUDGET_MS: how long a composition uploads tiles it
    // does not show (CoordinatedBackingStore::processPendingUpdates); 0
    // uploads every tile in the composition that receives it, as before.
    static const bigtime_t uploadBudget = [] {
        const char* value = std::getenv("SUMMIT_TILE_UPLOAD_BUDGET_MS");
        return static_cast<bigtime_t>((value && *value ? std::max(0.0, std::atof(value)) : 3.0) * 1000);
    }();
    CoordinatedBackingStore::setUploadDeadlineHaiku(uploadBudget ? system_time() + uploadBudget : 0);
    CoordinatedBackingStore::setViewportHaiku(IntRect({ }, viewportSize));
#endif
    WTFBeginSignpost(this, FlushCompositingState);
    flushCompositingState(reasons);
    WTFEndSignpost(this, FlushCompositingState);
#if PLATFORM(HAIKU)
    const bigtime_t flushedAt = traceTiming ? system_time() : 0;
    const auto tileUpdates = CoordinatedBackingStoreTile::takeUpdateStatsHaiku();
    const bool deferredUploads = CoordinatedBackingStore::takeHasDeferredUploadsHaiku();
#endif

    WTFBeginSignpost(this, PaintToGLContext);
    const auto targetContents = paintToCurrentGLContext(viewportTransform, viewportSize, reasons);
    WTFEndSignpost(this, PaintToGLContext);
#if PLATFORM(HAIKU)
    const bigtime_t paintedAt = traceTiming ? system_time() : 0;
#if USE(TEXTURE_MAPPER)
    const auto drawStats = TextureMapper::takeDrawStatsHaiku();
#endif
    unsigned texturelessTiles = 0;
    const unsigned waitingTiles = WebCore::summitTakeTileHolesHaiku(texturelessTiles);
#endif

    updateFPSCounter();

    if (shouldNotifiyDidComposite)
        m_didCompositeRunLoopObserver->schedule(&RunLoop::mainSingleton());

    WTFEmitSignpost(this, DidRenderFrame, "reasons: %s", reasonsToString(reasons).ascii().data());

#if USE(TEXTURE_MAPPER)
    if (m_context)
        m_context->swapBuffers();
#else
    PlatformDisplay::sharedDisplay().skiaGLContext()->swapBuffers();
#endif

#if PLATFORM(HAIKU)
    const bigtime_t swappedAt = traceTiming ? system_time() : 0;
#endif
    m_surface->didRenderFrame(targetContents);
#if PLATFORM(HAIKU)
    const bigtime_t readAt = traceTiming ? system_time() : 0;
#endif
    m_surface->sendFrame();
#if PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)
    m_lastFrameSentHaiku = MonotonicTime::now();
    if (m_renderCountHaiku == 1)
        SUMMIT_STARTUP_TRACE("first frame sent");
    if (!m_shaderWarmupStartedHaiku && (tileUpdates.tiles || m_renderCountHaiku >= 30)) {
        m_shaderWarmupStartedHaiku = true;
        SUMMIT_STARTUP_TRACE("first frame with tiles sent");
        static const bool warmsShaders = [] {
            const char* value = getenv("SUMMIT_WARM_SHADERS");
            return !value || strcmp(value, "0");
        }();
        // 16 programs after the first two; the TextureMapper stops earlier
        // once its budget is spent.
        if (warmsShaders) {
            m_workQueue->dispatch([this, protectedThis = Ref { *this }] {
                warmShaderProgramsHaiku(16, true);
            });
        }
    }
#endif
#if PLATFORM(HAIKU)
    if (traceTiming)
        std::fprintf(stderr, "Summit compositor timing: pid=%d at=%lld async=%d update=%d surface=%.1f flush=%.1f paint=%.1f send=%.1f total=%.1f tiles=%u newTextures=%u tileMpx=%.2f paintWait=%.1f texture=%.1f upload=%.1f swap=%.1f read=%.1f ipc=%.1f deferred=%d draws=%u blended=%u intoSurfaces=%u surfaces=%u blurs=%u drawMpx=%.1f blendedMpx=%.1f transparentTiles=%u waitingInView=%u texturelessInView=%u\n",
            static_cast<int>(getpid()), static_cast<long long>(startedAt), reasons.contains(CompositionReason::AsyncScrolling),
            reasons.contains(CompositionReason::RenderingUpdate), (surfaceAt - startedAt) / 1000.0,
            (flushedAt - surfaceAt) / 1000.0, (paintedAt - flushedAt) / 1000.0,
            (system_time() - paintedAt) / 1000.0, (system_time() - startedAt) / 1000.0,
            tileUpdates.tiles, tileUpdates.newTextures, tileUpdates.megapixels, tileUpdates.paintWaitMs, tileUpdates.textureMs, tileUpdates.uploadMs,
            (swappedAt - paintedAt) / 1000.0, (readAt - swappedAt) / 1000.0, (system_time() - readAt) / 1000.0, deferredUploads,
            drawStats.draws, drawStats.blendedDraws, drawStats.drawsIntoSurfaces, drawStats.surfaceBinds, drawStats.blurPasses, drawStats.megapixels, drawStats.blendedMegapixels, tileUpdates.transparentTiles, waitingTiles, texturelessTiles);
    // The tiles that were left over are uploaded by the compositions that
    // follow, which this asks for in case nothing else does.
    if (deferredUploads)
        requestComposition(CompositionReason::Animation);
#endif

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->didRenderFrame();
    });
}

void ThreadedCompositor::requestCompositionForRenderingUpdate(Function<void()>&& didCompositeFunction)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_state.lock };
    m_state.reasons.add(CompositionReason::RenderingUpdate);
    ASSERT(!m_state.didCompositeRenderingUpdateFunction);
    m_state.didCompositeRenderingUpdateFunction = WTF::move(didCompositeFunction);
    if (m_sceneState->pendingTiles())
        m_state.isWaitingForTiles = true;
    scheduleUpdateLocked();
}

void ThreadedCompositor::requestComposition(CompositionReason reason)
{
    Locker locker { m_state.lock };
    m_state.reasons.add(reason);
#if PLATFORM(HAIKU)
    if (traceCompositorTiming())
        std::fprintf(stderr, "Summit compositor request: pid=%d at=%lld async=%d update=%d state=%d waitingTiles=%d pendingTiles=%u timer=%d\n",
            static_cast<int>(getpid()), static_cast<long long>(system_time()), reason == CompositionReason::AsyncScrolling,
            reason == CompositionReason::RenderingUpdate, static_cast<int>(m_state.state),
            m_state.isWaitingForTiles, m_sceneState->pendingTiles(), m_state.isRenderTimerActive);
#endif
    scheduleUpdateLocked();
}

ASCIILiteral ThreadedCompositor::stateToString(ThreadedCompositor::State state)
{
    switch (state) {
    case State::Idle:
        return "Idle"_s;
    case State::Scheduled:
        return "Scheduled"_s;
    case State::InProgress:
        return "InProgress"_s;
    case State::ScheduledWhileInProgress:
        return "ScheduledWhileInProgress"_s;
    case State::Invalidated:
        return "Invalidated"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ThreadedCompositor::scheduleUpdateLocked()
{
    assertIsHeld(m_state.lock);
    WTFEmitSignpost(this, ScheduleComposition, "reasons: %s, state: %s, waiting for tiles: %s, render timer active: %s", reasonsToString(m_state.reasons).ascii().data(), stateToString(m_state.state).characters(), m_state.isWaitingForTiles ? "yes" : "no", m_state.isRenderTimerActive ? "yes" : "no");

    switch (m_state.state) {
    case State::Idle:
        m_state.state = State::Scheduled;
        if (!m_state.isWaitingForTiles && !m_suspendedCount.load())
            startRenderTimer();
        break;
    case State::Scheduled:
        if (!m_state.isRenderTimerActive && !m_suspendedCount.load())
            startRenderTimer();
        break;
    case State::InProgress:
        m_state.state = State::ScheduledWhileInProgress;
        break;
    case State::ScheduledWhileInProgress:
    case State::Invalidated:
        break;
    }
}

void ThreadedCompositor::frameComplete()
{
    ASSERT(m_workQueue->runLoop().isCurrent());
#if PLATFORM(HAIKU)
    if (testStallHaiku(TestStallHaiku::Frame))
        return;
#endif

    Locker locker { m_state.lock };
    WTFEmitSignpost(this, FrameComplete, "reasons: %s, state: %s, waiting for tiles: %s", reasonsToString(m_state.reasons).ascii().data(), stateToString(m_state.state).characters(), m_state.isWaitingForTiles ? "yes" : "no");

    switch (m_state.state) {
    case State::Idle:
    case State::Scheduled:
    case State::Invalidated:
        break;
    case State::InProgress:
        if (m_state.reasons.contains(CompositionReason::RenderingUpdate) && m_state.isWaitingForTiles)
            m_state.state = State::Scheduled;
        else
            m_state.state = State::Idle;
        break;
    case State::ScheduledWhileInProgress:
        m_state.state = State::Scheduled;
        if (!isOnlyRenderingUpdatePendingAndWaitingForTiles() && !m_suspendedCount.load())
            startRenderTimer();
        break;
    }
}

#if PLATFORM(HAIKU)
void ThreadedCompositor::renderTargetReleased()
{
    ASSERT(m_workQueue->runLoop().isCurrent());

    Locker locker { m_state.lock };
    if (!std::exchange(m_isWaitingForRenderTarget, false))
        return;
    // The render timer stayed active while waiting; run it now.
    if (m_state.isRenderTimerActive && !m_suspendedCount.load())
        m_renderTimer.startOneShot(0_s);
}

// The page has asked for a composition and waited |waited| for it, which
// takes a frame or two. Pages on the X399 have stopped drawing for good while
// they kept running (keys typed into GitHub's issue form showed only after a
// tab switch, whose suspend() and resume() restart this pipeline). Say where
// the composition is stuck, then restart it from there: a composition that is
// due is rendered at once instead of waiting for its timer (the first report,
// on 1 October, found the timer armed and a minute overdue), tiles that never
// report back stop holding it, and a frame the UI process never confirmed
// counts as shown.
void ThreadedCompositor::recoverFromStallHaiku(Seconds waited)
{
    ASSERT(RunLoop::isMain());
    auto now = MonotonicTime::now();
    if (now - m_lastStallCheckHaiku < 1_s)
        return;
    m_lastStallCheckHaiku = now;
    if (m_stallCheckPendingHaiku.exchange(true)) {
        std::fprintf(stderr, "Summit compositor stall: pid=%d the compositor thread has run no task for %.1f s\n",
            static_cast<int>(getpid()), (now - m_stallCheckPostedAtHaiku).seconds());
        return;
    }
    m_stallCheckPostedAtHaiku = now;
    m_workQueue->dispatch([this, protectedThis = Ref { *this }, waited] {
        m_stallCheckPendingHaiku = false;
        bool completeFrame = false;
        bool renderNow = false;
        {
            Locker locker { m_state.lock };
            auto now = MonotonicTime::now();
            bool inProgress = m_state.state == State::InProgress || m_state.state == State::ScheduledWhileInProgress;
            std::fprintf(stderr, "Summit compositor stall: pid=%d page waited %.1f s; state=%s reasons=0x%x render timer %s, suspended=%u, waiting for tiles=%d (%u pending), waiting for a buffer=%d, did-composite pending=%d, frame in progress for %.1f s; render timer %s\n",
                static_cast<int>(getpid()), waited.seconds(), stateToString(m_state.state).characters(), static_cast<unsigned>(m_state.reasons.toRaw()),
                m_state.isRenderTimerActive ? "active" : "inactive", m_suspendedCount.load(),
                m_state.isWaitingForTiles, m_sceneState->pendingTiles(), m_isWaitingForRenderTarget,
                !!m_state.didCompositeRenderingUpdateFunction, inProgress ? (now - m_inProgressSinceHaiku).seconds() : 0.0,
                describeTimerHaiku(m_renderTimer).utf8().data());
            // An update the page asked for that nothing scheduled.
            if (m_state.state == State::Idle && m_state.didCompositeRenderingUpdateFunction) {
                m_state.reasons.add(CompositionReason::RenderingUpdate);
                scheduleUpdateLocked();
            }
            switch (m_state.state) {
            case State::Scheduled:
                // Composite now: the render timer is what did not fire.
                if (m_suspendedCount.load())
                    break;
                m_state.isWaitingForTiles = false;
                m_state.isRenderTimerActive = true;
                renderNow = true;
                break;
            case State::InProgress:
            case State::ScheduledWhileInProgress:
                completeFrame = now - m_inProgressSinceHaiku >= 1_s;
                break;
            case State::Idle:
            case State::Invalidated:
                break;
            }
        }
        if (completeFrame)
            frameComplete();
        if (renderNow) {
            m_renderTimer.stop();
            renderLayerTree();
        }
    });
}

// SUMMIT_TEST_COMPOSITOR_STALL=timer, =late or =frame, once after the first 100
// compositions, leaves the render timer unarmed, arms it an hour late (what
// the owner's stalled tab showed), or loses a frame confirmation: the stalls
// recoverFromStallHaiku() has to undo, made on purpose to check that it does.
bool ThreadedCompositor::testStallHaiku(TestStallHaiku kind)
{
    static const std::optional<TestStallHaiku> requested = []() -> std::optional<TestStallHaiku> {
        const char* value = std::getenv("SUMMIT_TEST_COMPOSITOR_STALL");
        if (value && !std::strcmp(value, "timer"))
            return TestStallHaiku::Timer;
        if (value && !std::strcmp(value, "late"))
            return TestStallHaiku::Late;
        if (value && !std::strcmp(value, "frame"))
            return TestStallHaiku::Frame;
        return std::nullopt;
    }();
    static std::atomic<bool> done { false };
    if (requested != kind || m_renderCountHaiku < 100 || done.exchange(true))
        return false;
    std::fprintf(stderr, "Summit compositor test: pid=%d losing the %s after composition %u\n", static_cast<int>(getpid()),
        kind == TestStallHaiku::Timer ? "render timer" : kind == TestStallHaiku::Late ? "render timer (armed an hour late)" : "frame confirmation", m_renderCountHaiku);
    return true;
}
#endif

RunLoop* ThreadedCompositor::runLoop()
{
    return m_surface ? &m_workQueue->runLoop() : nullptr;
}

void ThreadedCompositor::didCompositeRunLoopObserverFired()
{
    m_didCompositeRunLoopObserver->invalidate();
    Function<void()> didCompositeFunction;
    {
        Locker locker { m_state.lock };
        didCompositeFunction = std::exchange(m_state.didCompositeRenderingUpdateFunction, nullptr);
    }
    if (didCompositeFunction)
        didCompositeFunction();
}

void ThreadedCompositor::updateSceneAttributes(const IntSize& size, float deviceScaleFactor)
{
    m_attributes.viewportSize = size;
    m_attributes.deviceScaleFactor = deviceScaleFactor;
    m_attributes.viewportSize.scale(m_attributes.deviceScaleFactor);
}

void ThreadedCompositor::initializeFPSCounter()
{
    // When the envvar is set, the FPS is logged to the console, so it may be necessary to enable the
    // 'LogsPageMessagesToSystemConsole' runtime preference to see it.
    const auto showFPSEnvironment = String::fromLatin1(getenv("WEBKIT_SHOW_FPS"));
    bool ok = false;
    Seconds interval(showFPSEnvironment.toDouble(&ok));
    if (ok && interval) {
        m_fpsCounter.exposesFPS = true;
        m_fpsCounter.calculationInterval = interval;
    }

    // WEBKIT_DRAW_FPS=1 additionally renders the FPS as an on-screen overlay,
    // reusing the calculation interval (which WEBKIT_SHOW_FPS may override).
    if (const auto* drawFPSEnvironment = getenv("WEBKIT_DRAW_FPS")) {
        if (auto enabled = parseInteger<unsigned>(StringView::fromLatin1(drawFPSEnvironment)); enabled && *enabled) {
            m_fpsCounter.exposesFPS = true;
            m_fpsCounter.drawsFPS = true;
        }
    }
}

void ThreadedCompositor::updateFPSCounter()
{
    if (!m_fpsCounter.exposesFPS
#if USE(SYSPROF_CAPTURE)
        && !SysprofAnnotator::singletonIfCreated()
#endif
    )
        return;

    m_fpsCounter.frameCountSinceLastCalculation++;
    const Seconds delta = MonotonicTime::now() - m_fpsCounter.lastCalculationTimestamp;
    if (delta >= m_fpsCounter.calculationInterval) {
        m_fpsCounter.lastFPS = static_cast<int>(std::round(m_fpsCounter.frameCountSinceLastCalculation / delta.seconds()));
        WTFSetCounter(FPS, m_fpsCounter.lastFPS);
        if (m_fpsCounter.exposesFPS)
            m_fpsCounter.fps = m_fpsCounter.frameCountSinceLastCalculation / delta.seconds();
        m_fpsCounter.frameCountSinceLastCalculation = 0;
        m_fpsCounter.lastCalculationTimestamp += delta;
    } else if (m_fpsCounter.exposesFPS)
        m_fpsCounter.fps = std::nullopt;
}

static const SkFont& fpsCounterFont()
{
    static SkFont font = [] {
        constexpr unsigned defaultFontSize = 14;
        unsigned fontSize = defaultFontSize;
        if (const auto* fontSizeEnvvar = getenv("WEBKIT_DRAW_FPS_FONT_SIZE")) {
            if (auto value = parseInteger<unsigned>(StringView::fromLatin1(fontSizeEnvvar)); value && *value)
                fontSize = *value;
        }
        auto typeface = FontCache::forCurrentThread().fontManager().matchFamilyStyle("monospace", SkFontStyle::Bold());
        SkFont f(typeface, fontSize);
        f.setEdging(SkFont::Edging::kAntiAlias);
        f.setSubpixel(true);
        return f;
    }();
    return font;
}

// Scale the box padding with the font size so the overlay stays proportionate at large
// WEBKIT_DRAW_FPS_FONT_SIZE values (~3px at the default size of 14).
static float fpsCounterPadding()
{
    return fpsCounterFont().getSize() * 0.2f;
}

FloatRect ThreadedCompositor::fpsCounterRect() const
{
    return FloatRect(0, 0, m_fpsCounter.backgroundWidth, m_fpsCounter.backgroundHeight);
}

void ThreadedCompositor::updateFPSCounterGeometry()
{
    if (m_fpsCounter.lastFPS == m_fpsCounter.displayedFPS)
        return;

    m_fpsCounter.displayedFPS = m_fpsCounter.lastFPS;
    m_fpsCounter.fpsString = String::number(m_fpsCounter.lastFPS).ascii();

    const auto& font = fpsCounterFont();
    SkRect textBounds;
    font.measureText(m_fpsCounter.fpsString.data(), m_fpsCounter.fpsString.length(), SkTextEncoding::kUTF8, &textBounds);

    const float padding = fpsCounterPadding();
    m_fpsCounter.backgroundWidth = textBounds.width() + padding * 2;
    m_fpsCounter.backgroundHeight = textBounds.height() + padding * 2;
    m_fpsCounter.textBaseline = -textBounds.fTop + padding;
}

#if ENABLE(DAMAGE_TRACKING)
IntRect ThreadedCompositor::takeFPSCounterDamage()
{
    // Nothing to repaint on the frames between counts, and drawFPSCounter() redraws the box regardless.
    if (m_fpsCounter.lastFPS == m_fpsCounter.displayedFPS)
        return { };

    updateFPSCounterGeometry();

    // The box drawn last frame is damaged too, since it shrinks when the count gets shorter.
    auto damage = enclosingIntRect(fpsCounterRect());
    damage.unite(std::exchange(m_fpsCounter.lastDrawnRect, damage));
    return damage;
}
#endif

void ThreadedCompositor::drawFPSCounter(SkCanvas& canvas)
{
    updateFPSCounterGeometry();

    // Drawn in device space at the top-left corner, matching the debug repaint
    // counter style used by SkiaCompositingLayer.
    SkAutoCanvasRestore autoRestore(&canvas, true);
    canvas.resetMatrix();

    SkPaint backgroundPaint;
    backgroundPaint.setColor(SK_ColorBLACK);
    backgroundPaint.setStyle(SkPaint::kFill_Style);
    canvas.drawRect(SkRect(fpsCounterRect()), backgroundPaint);

    SkPaint textPaint;
    textPaint.setColor(SK_ColorWHITE);
    textPaint.setAntiAlias(true);
    canvas.drawString(m_fpsCounter.fpsString.data(), fpsCounterPadding(), m_fpsCounter.textBaseline, fpsCounterFont(), textPaint);
}

void ThreadedCompositor::fillGLInformation(RenderProcessInfo&& info, CompletionHandler<void(RenderProcessInfo&&)>&& completionHandler)
{
    m_workQueue->dispatchSync([protectedThis = Ref { *this }, info = WTF::move(info), completionHandler = WTF::move(completionHandler)]() mutable {
        info.glRenderer = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        info.glVendor = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        info.glVersion = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        info.glShadingVersion = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
        info.glExtensions = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)));

        auto eglDisplay = eglGetCurrentDisplay();
        info.eglVersion = String::fromUTF8(eglQueryString(eglDisplay, EGL_VERSION));
        info.eglVendor = String::fromUTF8(eglQueryString(eglDisplay, EGL_VENDOR));
        info.eglExtensions = makeString(unsafeSpan(eglQueryString(nullptr, EGL_EXTENSIONS)), ' ', unsafeSpan(eglQueryString(eglDisplay, EGL_EXTENSIONS)));

        RunLoop::mainSingleton().dispatch([info = WTF::move(info), completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler(WTF::move(info));
        });
    });
}

void ThreadedCompositor::releaseMemory(WTF::Critical critical)
{
    m_workQueue->dispatchSync([protectedThis = Ref { *this }, critical] {
        PlatformDisplay::sharedDisplay().skiaReleaseUnusedResources(critical);
    });
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
