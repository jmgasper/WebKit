/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Company 100, Inc.
 * Copyright (C) 2014-2019 Igalia S.L.
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
#include "LayerTreeHost.h"
#include <WebCore/LayerTreeAsTextOptions.h>

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedSceneState.h"
#include "DrawingArea.h"
#include "RenderProcessInfo.h"
#include "WebPageInlines.h"
#include "WebPageProxyMessages.h"
#include "WebProcess.h"
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/Chrome.h>
#include <WebCore/Damage.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PageOverlayController.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/ProcessCapabilities.h>
#include <WebCore/RenderLayerBacking.h>
#include <WebCore/RenderView.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/Settings.h>
#include <WebCore/SkiaPaintingEngine.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <WebCore/WindowEventLoop.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/MonotonicTime.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WallTime.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/text/StringToIntegerConversion.h>
#if PLATFORM(HAIKU)
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <WebCore/RenderingStallHaiku.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHost);

#if ENABLE(DAMAGE_TRACKING)
static bool damageOverlayForcesPropagation()
{
    // WEBKIT_SHOW_DAMAGE draws the damage the layers report, so it needs propagation on even when the
    // propagateDamagingInformation setting that normally turns it on is off. Only the Skia accumulated-damage
    // overlay is handled here. The non-Skia TextureMapperDamageVisualizer draws off the propagation flags instead.
#if USE(TEXTURE_MAPPER)
    return false;
#else
    const auto* showDamageEnvvar = getenv("WEBKIT_SHOW_DAMAGE");
    if (!showDamageEnvvar)
        return false;

    auto value = parseInteger<unsigned>(StringView::fromLatin1(showDamageEnvvar));
    return value && *value;
#endif
}
#endif

std::unique_ptr<LayerTreeHost> LayerTreeHost::create(WebPage& webPage)
{
    return makeUnique<LayerTreeHost>(webPage);
}

LayerTreeHost::LayerTreeHost(WebPage& webPage)
    : m_webPage(webPage)
    , m_sceneState(CoordinatedSceneState::create())
#if PLATFORM(HAIKU)
    , m_stallWatchdogHaiku(RunLoop::mainSingleton(), "LayerTreeHost::StallWatchdog"_s, this, &LayerTreeHost::checkRenderingStallHaiku)
    , m_livenessTimerHaiku(RunLoop::mainSingleton(), "LayerTreeHost::Liveness"_s, this, &LayerTreeHost::checkLivenessHaiku)
    , m_lastRenderingUpdateHaiku(MonotonicTime::now())
#endif
{
#if PLATFORM(HAIKU)
    m_livenessTimerHaiku.startRepeating(1_s);
#endif
    {
        auto& rootLayer = m_sceneState->rootLayer();
#if ENABLE(DAMAGE_TRACKING)
        const auto& settings = webPage.corePage()->settings();
        const bool propagateDamage = settings.propagateDamagingInformation() || damageOverlayForcesPropagation();
        rootLayer.setDamagePropagationEnabled(propagateDamage);
        if (propagateDamage) {
            m_damageInGlobalCoordinateSpace = std::make_shared<Damage>(m_webPage->size());
            rootLayer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
        }
#endif
        Locker locker { rootLayer.lock() };
        rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
        rootLayer.setSize(m_webPage->size());
    }

    m_compositor = ThreadedCompositor::create(webPage, *this, m_sceneState.get());
#if USE(TEXTURE_MAPPER)
    m_skiaPaintingEngine = SkiaPaintingEngine::create(nullptr);
#else
    m_skiaPaintingEngine = SkiaPaintingEngine::create(m_compositor->threadSafeGrContext());
#endif

#if ENABLE(DAMAGE_TRACKING)
    std::optional<OptionSet<ThreadedCompositor::DamagePropagationFlags>> damagePropagationFlags;
    const auto& settings = webPage.corePage()->settings();
    if (settings.propagateDamagingInformation()) {
        damagePropagationFlags = OptionSet<ThreadedCompositor::DamagePropagationFlags> { };
        if (settings.unifyDamagedRegions())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::Unified);
        if (settings.useDamagingInformationForCompositing())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::UseForCompositing);
    }
    m_compositor->setDamagePropagationSettings(damagePropagationFlags, settings.damageRectangleThreshold());
#endif
}

LayerTreeHost::~LayerTreeHost()
{
    m_skiaPaintingEngine = nullptr;

    // ThreadedCompositor must be invalidated before invalidating CoordinatedSceneState
    // to invalidate pending layers in the compositor thread.
    m_compositor->invalidate();
    m_sceneState->invalidate();
}

uint64_t LayerTreeHost::surfaceID() const
{
    return m_compositor->surfaceID();
}

void LayerTreeHost::scheduleRenderingUpdate()
{
    WTFEmitSignpost(this, LayerTreeHostScheduleRenderingUpdate, "isWaitingForRenderer %s", m_isWaitingForRenderer ? "yes" : "no");

#if PLATFORM(HAIKU)
    m_lastScheduleRequestHaiku = MonotonicTime::now();
    static bool traceLayerUpdates = [] {
        const char* value = std::getenv("SUMMIT_LAYER_UPDATE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    if (traceLayerUpdates)
        std::fprintf(stderr, "Summit layer update: schedule waiting=%d queued=%d frozen=%d suspended=%d\n",
            m_isWaitingForRenderer, m_scheduledWhileWaitingForRenderer, m_layerTreeStateIsFrozen, m_isSuspended);
#endif

    if (m_layerTreeStateIsFrozen || m_isSuspended || m_webPage->size().isEmpty())
        return;

    if (m_isWaitingForRenderer) {
        m_scheduledWhileWaitingForRenderer = true;
#if PLATFORM(HAIKU)
        checkRenderingStallHaiku();
#endif
        return;
    }

    scheduleRenderingUpdateRunLoopObserver();
}

#if PLATFORM(HAIKU)
// A page that still runs but has waited seconds for the composition it asked
// for has stopped drawing (typing showed only after a tab switch). Asking for
// another update while one is pending does not come back here, so the first
// request that finds the page waiting starts a watchdog that checks every
// second until a composition arrives. The compositor first says why it is
// stuck and restarts itself; if the page is still waiting two seconds later,
// it stops waiting.
void LayerTreeHost::checkRenderingStallHaiku()
{
    if (!m_isWaitingForRenderer || m_isSuspended || m_layerTreeStateIsFrozen) {
        m_stallRecoveryStartedAtHaiku = std::nullopt;
        return;
    }
    auto now = MonotonicTime::now();
    Seconds waited = now - m_waitingForRendererSinceHaiku;
    if (waited < 2_s) {
        if (!m_stallWatchdogHaiku.isActive())
            m_stallWatchdogHaiku.startOneShot(2_s - waited);
        return;
    }
    if (!m_stallRecoveryStartedAtHaiku)
        m_stallRecoveryStartedAtHaiku = now;
    if (now - *m_stallRecoveryStartedAtHaiku < 2_s) {
        m_compositor->recoverFromStallHaiku(waited);
        m_stallWatchdogHaiku.startOneShot(1_s);
        return;
    }
    std::fprintf(stderr, "Summit layer tree stall: pid=%d no composition for %.1f s; updating the page without it\n",
        static_cast<int>(getpid()), waited.seconds());
    m_stallRecoveryStartedAtHaiku = std::nullopt;
    m_isWaitingForRenderer = false;
    m_scheduledWhileWaitingForRenderer = false;
    scheduleRenderingUpdateRunLoopObserver();
}

// Once a second: a page in view that something asked to draw but that has had
// no rendering update for two seconds has stalled somewhere between WebCore's
// refresh timer, this host's update observer and the compositor (typed text
// on GitHub showed only after a tab switch, and the stall report above never
// came: nothing reached it). Say what is pending at each step, once per
// stall, then restart each of them.
void LayerTreeHost::checkLivenessHaiku()
{
    Ref webPage = m_webPage;
    RefPtr corePage = webPage->corePage();
    if (!corePage || m_isUpdatingRendering || !webPage->isVisible()) {
        m_blockedInViewSinceHaiku = std::nullopt;
        return;
    }
    auto now = MonotonicTime::now();
    if (m_isSuspended || m_layerTreeStateIsFrozen) {
        // A page in view whose rendering stays suspended (or frozen longer
        // than a slow load would keep it) draws nothing. Only reported: the
        // drawing area keeps its own record of the suspension.
        if (!m_blockedInViewSinceHaiku)
            m_blockedInViewSinceHaiku = now;
        Seconds blocked = now - *m_blockedInViewSinceHaiku;
        // A page that a navigation to another site left in the back/forward
        // cache (WebPage::suspendForProcessSwap()) is frozen for good and
        // still thinks it is in view; it is not the one being shown.
        auto freezeReasons = webPage->layerTreeFreezeReasons();
        if (freezeReasons.contains(WebPage::LayerTreeFreezeReason::PageSuspended))
            return;
        if (blocked >= (m_isSuspended ? 2_s : 10_s) && !std::exchange(m_reportedLivenessStallHaiku, true)) {
            std::fprintf(stderr, "Summit page stall: pid=%d in view but rendering %s for %.1f s (freeze reasons %#x); WebCore scheduler: %s\n",
                static_cast<int>(getpid()), m_isSuspended ? "suspended" : "frozen", blocked.seconds(), freezeReasons.toRaw(),
                describeRenderingUpdateSchedulerHaiku(*corePage).utf8().data());
        }
        return;
    }
    m_blockedInViewSinceHaiku = std::nullopt;
    Seconds sinceUpdate = now - m_lastRenderingUpdateHaiku;
    if (sinceUpdate < 2_s)
        return;
    bool refreshPending = isRenderingUpdateSchedulerPendingHaiku(*corePage);
    bool requested = m_lastScheduleRequestHaiku > m_lastRenderingUpdateHaiku;
    bool observerScheduled = m_renderingUpdateRunLoopObserver->isScheduled();
    if (!refreshPending && !requested && !observerScheduled && !m_isWaitingForRenderer)
        return;
    if (!std::exchange(m_reportedLivenessStallHaiku, true)) {
        std::fprintf(stderr, "Summit page stall: pid=%d no rendering update for %.1f s; WebCore scheduler: %s; drawing asked for %s; update observer %s; waiting for the compositor %s\n",
            static_cast<int>(getpid()), sinceUpdate.seconds(), describeRenderingUpdateSchedulerHaiku(*corePage).utf8().data(),
            requested ? "since the last update" : "not since the last update", observerScheduled ? "pending" : "not pending",
            m_isWaitingForRenderer ? "yes" : "no");
    }
    if (m_isWaitingForRenderer) {
        checkRenderingStallHaiku();
        return;
    }
    if (refreshPending)
        restartRenderingUpdateSchedulerHaiku(*corePage);
    if (m_renderingUpdateRunLoopObserver->isScheduled())
        m_renderingUpdateRunLoopObserver->invalidate();
    scheduleRenderingUpdateRunLoopObserver();
}
#endif

void LayerTreeHost::scheduleRenderingUpdateRunLoopObserver()
{
    FrameRenderer::scheduleRenderingUpdateRunLoopObserver();

    // Avoid running any more tasks before the runloop observer fires.
    WindowEventLoop::breakToAllowRenderingUpdate();
}

bool LayerTreeHost::canUpdateRendering() const
{
    return !m_isWaitingForRenderer;
}

void LayerTreeHost::updateRendering()
{
#if PLATFORM(HAIKU)
    static bool traceRenderPhases = [] {
        const char* value = std::getenv("SUMMIT_RENDER_UPDATE_PHASE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    auto phaseStart = traceRenderPhases ? MonotonicTime::now() : MonotonicTime { };
    auto afterPageUpdate = phaseStart;
    auto afterFinalize = phaseStart;
    auto afterSceneFlush = phaseStart;
    static bool traceLayerUpdates = [] {
        const char* value = std::getenv("SUMMIT_LAYER_UPDATE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    if (traceLayerUpdates)
        std::fprintf(stderr, "Summit layer update: begin waiting=%d queued=%d\n",
            m_isWaitingForRenderer, m_scheduledWhileWaitingForRenderer);
#endif
    invalidateRenderingUpdateRunLoopObserver();

    RELEASE_ASSERT(!m_isUpdatingRendering);
    if (m_layerTreeStateIsFrozen)
        return;
#if PLATFORM(HAIKU)
    m_lastRenderingUpdateHaiku = MonotonicTime::now();
    m_reportedLivenessStallHaiku = false;
#endif

#if PLATFORM(HAIKU)
    // Runs after reentrancyProtector has been destroyed (declared before it).
    auto scheduleRequestedWhileUpdating = makeScopeExit([this] {
        if (!std::exchange(m_renderingUpdateRequestedWhileUpdatingHaiku, false))
            return;
        static bool traceLayerUpdates = [] {
            const char* value = std::getenv("SUMMIT_LAYER_UPDATE_TRACE");
            return value && value[0] == '1' && !value[1];
        }();
        if (traceLayerUpdates)
            std::fprintf(stderr, "Summit layer update: requested while updating\n");
        scheduleRenderingUpdate();
    });
#endif
    SetForScope<bool> reentrancyProtector(m_isUpdatingRendering, true);


    TraceScope traceScope(LayerTreeHostRenderingUpdateStart, LayerTreeHostRenderingUpdateEnd);

    Ref page = m_webPage;
    page->updateRendering();
#if PLATFORM(HAIKU)
    if (traceRenderPhases)
        afterPageUpdate = MonotonicTime::now();
#endif
    page->flushPendingEditorStateUpdate();
#if PLATFORM(MAC) || PLATFORM(WPE) || PLATFORM(GTK)
    // ThemeColorChanged is declared for those ports only.
    page->flushPendingThemeColorChange();
#endif

    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(visibleContentsRect());

    OptionSet<FinalizeRenderingUpdateFlags> flags;
#if PLATFORM(GTK)
    if (!m_transientZoom)
        flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#else
    flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#endif
    page->finalizeRenderingUpdate(flags);
#if PLATFORM(HAIKU)
    if (traceRenderPhases)
        afterFinalize = MonotonicTime::now();
    // SUMMIT_LAYER_TREE_DUMP=1: the page's layer tree as text (positions,
    // sizes, what each layer draws, opacity, painting phases), whenever it
    // changed, at most once a second. Finds what the compositor is told to
    // draw when a page shows less than it should.
    static const bool dumpLayerTree = [] {
        const char* value = getenv("SUMMIT_LAYER_TREE_DUMP");
        return value && value[0] == '1' && !value[1];
    }();
    if (dumpLayerTree && m_rootCompositingLayer) {
        static MonotonicTime lastDump;
        static String lastTree;
        if (MonotonicTime::now() - lastDump > 1_s) {
            lastDump = MonotonicTime::now();
            auto tree = m_rootCompositingLayer->layerTreeAsText({ WebCore::LayerTreeAsTextOptions::Debug, WebCore::LayerTreeAsTextOptions::IncludeVisibleRects,
                WebCore::LayerTreeAsTextOptions::IncludePaintingPhases, WebCore::LayerTreeAsTextOptions::IncludeContentLayers,
                WebCore::LayerTreeAsTextOptions::IncludeClipping, WebCore::LayerTreeAsTextOptions::IncludeRootLayerProperties });
            if (tree != lastTree) {
                lastTree = tree;
                fprintf(stderr, "Summit layer tree (pid %d):\n%s\n", getpid(), tree.utf8().legacyCStringPointer());
            }
        }
    }
#endif

    if (m_pendingResize) {
        m_compositor->setSize(page->size(), page->deviceScaleFactor());
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(page->size());
    }

#if PLATFORM(GTK)
    // If we have an active transient zoom, we want the zoom to win over any changes
    // that WebCore makes to the relevant layers, so re-apply our changes after flushing.
    if (m_transientZoom)
        applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);
#endif

    if (RefPtr drawingArea = page->drawingArea())
        drawingArea->dispatchPendingCallbacksAfterEnsuringDrawing();

    bool didChangeSceneState = m_sceneState->flush();
#if PLATFORM(HAIKU)
    if (traceRenderPhases)
        afterSceneFlush = MonotonicTime::now();
#endif
    if (m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState)
        requestCompositionForRenderingUpdate();

    m_compositionRequired = false;
    m_pendingResize = false;
    m_forceFrameSync = false;

    page->didUpdateRendering();

    // Eject any backing stores whose only reference is held in the HashMap cache.
    m_imageBackingStores.removeIf([](auto& it) {
        return it.value->hasOneRef();
    });

    if (m_waitUntilPaintingComplete) {
        m_sceneState->waitUntilPaintingComplete();
        m_waitUntilPaintingComplete = false;
    }
#if PLATFORM(HAIKU)
    if (traceRenderPhases) {
        auto finished = MonotonicTime::now();
        if (finished - phaseStart >= 2_ms)
            std::fprintf(stderr, "Summit render phases: wall=%.3f total=%.1f page=%.1f finalize=%.1f scene=%.1f finish=%.1f changed=%d\n",
                WallTime::now().secondsSinceEpoch().milliseconds(),
                (finished - phaseStart).milliseconds(), (afterPageUpdate - phaseStart).milliseconds(),
                (afterFinalize - afterPageUpdate).milliseconds(), (afterSceneFlush - afterFinalize).milliseconds(),
                (finished - afterSceneFlush).milliseconds(), didChangeSceneState);
    }
#endif
}

void LayerTreeHost::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootCompositingLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(m_rootCompositingLayer)->coordinatedPlatformLayer());
        if (m_overlayCompositingLayer)
            children.append(downcast<GraphicsLayerCoordinated>(m_overlayCompositingLayer)->coordinatedPlatformLayer());
    }

    m_sceneState->setRootLayerChildren(WTF::move(children));
}

void LayerTreeHost::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::updateRenderingWithForcedRepaint()
{
    if (m_isWaitingForRenderer) {
        if (m_forcedRepaintAsyncCallback)
            m_pendingForceRepaint = true;
        return;
    }

    m_pendingForceRepaint = false;
    protect(m_webPage)->corePage()->forceRepaintAllFrames();
    m_forceFrameSync = true;

    // Make sure `m_sceneState->waitUntilPaintingComplete()` is invoked at the
    // end of the currently running layer flush, or after the next one if there
    // is none ongoing at present.
    m_waitUntilPaintingComplete = true;

    // If updateRenderingWithForcedRepaint() is invoked via JS through e.g. a rAF() callback, a call
    // to `page->updateRendering()` _during_ a layer flush is responsible for that.
    // If m_isUpdatingRendering is true, that layer flush is still ongoing, so we do
    // not need to cancel pending ones and immediately flush again (re-entrancy!).
    if (m_isUpdatingRendering)
        return;
    updateRendering();
}

bool LayerTreeHost::ensureDrawing()
{
    if (m_layerTreeStateIsFrozen || m_isSuspended || m_webPage->size().isEmpty())
        return false;

    m_forceFrameSync = true;
    scheduleRenderingUpdate();
    return true;
}

void LayerTreeHost::sizeDidChange()
{
    m_pendingResize = true;
    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else
        updateRendering();
}

void LayerTreeHost::suspend()
{
    FrameRenderer::suspend();
    m_compositor->suspend();
}

void LayerTreeHost::resume()
{
#if PLATFORM(HAIKU)
    // Time spent hidden is not time spent waiting.
    m_waitingForRendererSinceHaiku = MonotonicTime::now();
    m_lastRenderingUpdateHaiku = m_waitingForRendererSinceHaiku;
    m_stallRecoveryStartedAtHaiku = std::nullopt;
#endif
    m_compositor->resume();
    FrameRenderer::resume();
}

GraphicsLayerFactory* LayerTreeHost::graphicsLayerFactory()
{
    return this;
}

FloatRect LayerTreeHost::visibleContentsRect() const
{
    Ref webPage = m_webPage;
    if (auto* localMainFrameView = webPage->localMainFrameView())
        return FloatRect({ }, localMainFrameView->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes));
    return webPage->bounds();
}

void LayerTreeHost::backgroundColorDidChange()
{
    m_compositor->backgroundColorDidChange();
}

void LayerTreeHost::attachLayer(CoordinatedPlatformLayer& layer)
{
#if ENABLE(DAMAGE_TRACKING)
    layer.setDamagePropagationEnabled(!!m_damageInGlobalCoordinateSpace);
    if (m_damageInGlobalCoordinateSpace)
        layer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
#endif
    m_sceneState->addLayer(layer);
}

void LayerTreeHost::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

void LayerTreeHost::notifyCompositionRequired()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_compositionRequiredInScrollingThread = true;
        return;
    }
#endif
    m_compositionRequired = true;
}

bool LayerTreeHost::isCompositionRequiredOrOngoing() const
{
    return m_compositionRequired || m_forceFrameSync || m_compositor->isActive();
}

void LayerTreeHost::requestComposition(CompositionReason reason)
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_sceneState->flushPendingState();
        if (!m_compositionRequiredInScrollingThread)
            return;
        m_compositionRequiredInScrollingThread = false;
    }
#endif

    m_compositor->requestComposition(reason);
}

RunLoop* LayerTreeHost::compositingRunLoop() const
{
    return m_compositor->runLoop();
}

int LayerTreeHost::maxTextureSize() const
{
    return m_compositor->maxTextureSize();
}

void LayerTreeHost::willPaintTile()
{
    m_sceneState->willPaintTile();
}

void LayerTreeHost::didPaintTile()
{
    m_sceneState->didPaintTile();
    m_compositor->pendingTilesDidChange();
}

Ref<CoordinatedImageBackingStore> LayerTreeHost::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto nativeImageID = nativeImage->uniqueID();
    auto addResult = m_imageBackingStores.ensure(nativeImageID, [&] {
#if USE(TEXTURE_MAPPER)
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage));
#else
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage), m_compositor->threadSafeGrContext());
#endif
    });
    return addResult.iterator->value;
}

Ref<GraphicsLayer> LayerTreeHost::createGraphicsLayer(GraphicsLayer::Type layerType, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(layerType, client, CoordinatedPlatformLayer::create(*this)));
}

void LayerTreeHost::willRenderFrame()
{
    if (RefPtr drawingArea = protect(m_webPage)->drawingArea())
        drawingArea->willStartRenderingUpdateDisplay();
}

void LayerTreeHost::didRenderFrame()
{
    Ref webPage = m_webPage;
    if (RefPtr drawingArea = webPage->drawingArea())
        drawingArea->didCompleteRenderingUpdateDisplay();
    if (auto fps = m_compositor->fps()) {
        if (RefPtr document = webPage->corePage()->localTopDocument())
            document->addConsoleMessage(MessageSource::Rendering, MessageLevel::Info, makeString("FPS: "_s, *fps));
    }
}

void LayerTreeHost::requestCompositionForRenderingUpdate()
{
#if PLATFORM(HAIKU)
    static bool traceLayerUpdates = [] {
        const char* value = std::getenv("SUMMIT_LAYER_UPDATE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    if (traceLayerUpdates)
        std::fprintf(stderr, "Summit layer update: request-composite\n");
#endif
    m_isWaitingForRenderer = true;
#if PLATFORM(HAIKU)
    m_waitingForRendererSinceHaiku = MonotonicTime::now();
    m_stallRecoveryStartedAtHaiku = std::nullopt;
#endif
    m_compositor->requestCompositionForRenderingUpdate([this] {
#if PLATFORM(HAIKU)
        if (traceLayerUpdates)
            std::fprintf(stderr, "Summit layer update: did-composite queued=%d\n", m_scheduledWhileWaitingForRenderer);
#endif
        WTFBeginSignpost(this, DidComposite);

        if (!m_pendingForceRepaint && m_forcedRepaintAsyncCallback)
            m_forcedRepaintAsyncCallback();

        m_isWaitingForRenderer = false;
        bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);
        if (m_pendingForceRepaint) {
            if (!m_layerTreeStateIsFrozen)
                updateRenderingWithForcedRepaint();
            else if (m_forcedRepaintAsyncCallback)
                m_forcedRepaintAsyncCallback();
        } else if (!m_isSuspended && !m_layerTreeStateIsFrozen && scheduledWhileWaitingForRenderer)
            scheduleRenderingUpdateRunLoopObserver();

        WTFEndSignpost(this, DidComposite);
    });
    WTFEmitSignpost(this, RequestCompositionForRenderingUpdate);
}

#if PLATFORM(GTK)
FloatPoint LayerTreeHost::constrainTransientZoomOrigin(double scale, FloatPoint origin) const
{
    Ref webPage = m_webPage;
    auto* frameView = webPage->localMainFrameView();
    if (!frameView)
        return origin;

    FloatRect visibleContentRect = frameView->visibleContentRectIncludingScrollbars();

    FloatPoint constrainedOrigin = visibleContentRect.location();
    constrainedOrigin.moveBy(-origin);

    IntSize scaledTotalContentsSize = frameView->totalContentsSize();
    scaledTotalContentsSize.scale(scale * webPage->viewScaleFactor() / webPage->totalScaleFactor());

    // Scaling may have exposed the overhang area, so we need to constrain the final
    // layer position exactly like scrolling will once it's committed, to ensure that
    // scrolling doesn't make the view jump.
    constrainedOrigin = ScrollableArea::constrainScrollPositionForOverhang(roundedIntRect(visibleContentRect),
        scaledTotalContentsSize, roundedIntPoint(constrainedOrigin), frameView->scrollOrigin(),
        frameView->headerHeight(), frameView->footerHeight());
    constrainedOrigin.moveBy(-visibleContentRect.location());
    constrainedOrigin = -constrainedOrigin;

    return constrainedOrigin;
}

CoordinatedPlatformLayer* LayerTreeHost::layerForTransientZoom() const
{
    auto* frameView = protect(m_webPage)->localMainFrameView();
    if (!frameView)
        return nullptr;

    RenderLayerBacking* renderViewBacking = frameView->renderView()->layer()->backing();
    if (!renderViewBacking)
        return nullptr;

    auto* scaledLayer = renderViewBacking->contentsContainmentLayer();
    if (!scaledLayer)
        scaledLayer = renderViewBacking->graphicsLayer();
    ASSERT(scaledLayer);
    return &downcast<GraphicsLayerCoordinated>(*scaledLayer).coordinatedPlatformLayer();
}

void LayerTreeHost::applyTransientZoomToLayers(double scale, FloatPoint origin)
{
    // FIXME: Scrollbars should stay in-place and change height while zooming.
    FloatPoint constrainedOrigin = constrainTransientZoomOrigin(scale, origin);
    auto* zoomLayer = layerForTransientZoom();

    TransformationMatrix transform;
    transform.translate(constrainedOrigin.x(), constrainedOrigin.y());
    transform.scale(scale);

    Locker locker { zoomLayer->lock() };
    zoomLayer->setTransform(transform);
    zoomLayer->setAnchorPoint(FloatPoint3D());
    zoomLayer->setPosition(FloatPoint());
}

void LayerTreeHost::adjustTransientZoom(double scale, FloatPoint origin, FloatPoint)
{
    m_transientZoom = true;
    m_transientZoomScale = scale;
    m_transientZoomOrigin = origin;

    applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);

    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else
        updateRendering();
}

void LayerTreeHost::commitTransientZoom(double scale, FloatPoint origin, FloatPoint unscrolledOrigin)
{
    if (m_transientZoomScale == scale) {
        // If the page scale is already the target scale, setPageScaleFactor() will short-circuit
        // and not apply the transform, so we can't depend on it to do so.
        TransformationMatrix finalTransform;
        finalTransform.scale(scale);

        auto* zoomLayer = layerForTransientZoom();
        Locker locker { zoomLayer->lock() };
        zoomLayer->setTransform(finalTransform);
    }

    m_transientZoom = false;
    m_transientZoomScale = 1;
    m_transientZoomOrigin = FloatPoint();

    Ref webPage = m_webPage;
    webPage->scalePage(scale / webPage->viewScaleFactor(), roundedIntPoint(-unscrolledOrigin));
}
#endif

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void LayerTreeHost::preferredBufferFormatsDidChange()
{
    m_compositor->preferredBufferFormatsDidChange();
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void LayerTreeHost::notifyFrameDamageForTesting(Region&& damageRegion)
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    m_frameDamageHistoryForTesting.append(WTF::move(damageRegion));
}

void LayerTreeHost::resetDamageHistoryForTesting()
{
    {
        Locker locker { m_frameDamageHistoryForTestingLock };
        m_frameDamageHistoryForTesting.clear();
    }
    m_compositor->enableFrameDamageNotificationForTesting();
}

void LayerTreeHost::foreachRegionInDamageHistoryForTesting(Function<void(const Region&)>&& callback) const
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    for (const auto& region : m_frameDamageHistoryForTesting)
        callback(region);
}
#endif

void LayerTreeHost::fillGLInformation(RenderProcessInfo&& info, CompletionHandler<void(RenderProcessInfo&&)>&& completionHandler)
{
#if USE(TEXTURE_MAPPER)
    if (ProcessCapabilities::canUseAcceleratedBuffers() && PlatformDisplay::sharedDisplay().skiaGLContext())
        info.gpuPaintingThreadsCount = SkiaPaintingEngine::numberOfGPUPaintingThreads();
    else
        info.cpuPaintingThreadsCount = SkiaPaintingEngine::numberOfCPUPaintingThreads();
#else
    auto threadsCount = SkiaPaintingEngine::numberOfCPUPaintingThreads();
    if (ProcessCapabilities::canUseAcceleratedBuffers())
        info.gpuPaintingThreadsCount = threadsCount;
    else
        info.cpuPaintingThreadsCount = threadsCount;
#endif
    m_compositor->fillGLInformation(WTF::move(info), WTF::move(completionHandler));
}

void LayerTreeHost::releaseMemory(WTF::Critical critical)
{
    PlatformDisplay::sharedDisplay().skiaReleaseUnusedResources(critical);
    m_compositor->releaseMemory(critical);
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
