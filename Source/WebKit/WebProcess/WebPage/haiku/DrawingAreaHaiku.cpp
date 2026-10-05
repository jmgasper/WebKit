/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "DrawingAreaHaiku.h"

#include "BitmapFrameHaiku.h"
#include "DrawingAreaProxyMessages.h"
#include "MessageSenderInlines.h"
#include "WebPage.h"
#include "WebPageCreationParameters.h"
#include "WebPageInlines.h"
#include <WebCore/GraphicsContext.h>
#include <WebCore/Page.h>
#include <WebCore/PaintStatisticsHaiku.h>
#include <WebCore/ShareableBitmap.h>
#include <Window.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#if USE(TEXTURE_MAPPER)
#include "LayerTreeHostHaiku.h"
#include <WebCore/Settings.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(DrawingAreaHaiku);

// The port has no display link, and app_server presents on its own 60 Hz clock.
// SUMMIT_FRAME_INTERVAL_MS overrides the target period for experiments; 0 paints
// as fast as the page and the UI process allow.
static Seconds displayPeriodHaiku()
{
    static const Seconds period = [] {
        if (const char* value = getenv("SUMMIT_FRAME_INTERVAL_MS")) {
            char* end = nullptr;
            double milliseconds = std::strtod(value, &end);
            if (end != value && std::isfinite(milliseconds) && milliseconds >= 0 && milliseconds <= 1000)
                return Seconds::fromMilliseconds(milliseconds);
        }
        return Seconds::fromMilliseconds(1000.0 / 60.0);
    }();
    return period;
}

Ref<DrawingAreaHaiku> DrawingAreaHaiku::create(WebPage& page, const WebPageCreationParameters& parameters)
{
    return adoptRef(*new DrawingAreaHaiku(page, parameters));
}

DrawingAreaHaiku::DrawingAreaHaiku(WebPage& page, const WebPageCreationParameters& parameters)
    : DrawingArea(parameters.drawingAreaIdentifier, page)
    , m_displayTimer(RunLoop::mainSingleton(), "DrawingAreaHaiku::Display"_s, this, &DrawingAreaHaiku::display)
    , m_isVisible(parameters.activityState.contains(ActivityState::IsVisible))
{
    if (const char* value = getenv("SUMMIT_FRAME_STATS")) {
        char* end = nullptr;
        double seconds = std::strtod(value, &end);
        if (end == value || !std::isfinite(seconds) || seconds <= 0)
            seconds = 1;
        if (seconds <= 60) {
            m_statistics = std::make_unique<FrameStatistics>();
            m_statistics->period = Seconds(seconds);
            m_statistics->periodStart = MonotonicTime::now();
            // Also tally what each frame asks the port to draw.
            WebCore::paintStatisticsHaiku().enabled = true;
        }
    }
}

DrawingAreaHaiku::~DrawingAreaHaiku()
{
    m_displayTimer.stop();
    completeRepaintCallbacks();
}

void DrawingAreaHaiku::scheduleFrame()
{
    m_needsDisplay = true;
    scheduleDisplay();
}

// Both paths only repaint invalidated content: the software path into the frame
// bitmap it keeps between frames, the GL path into its non-composited tiles.
void DrawingAreaHaiku::setNeedsDisplay()
{
    m_needsFullRepaint = true;
    m_scrollRect = { };
    m_scrollOffset = { };
#if USE(TEXTURE_MAPPER)
    if (m_layerTreeHost)
        m_layerTreeHost->setNonCompositedContentsNeedDisplay();
#endif
    scheduleFrame();
}

void DrawingAreaHaiku::setNeedsDisplayInRect(const IntRect& rect)
{
    if (!rect.intersects(m_webPage->bounds()))
        return;
    m_dirtyRegion.unite(intersection(rect, m_webPage->bounds()));
#if USE(TEXTURE_MAPPER)
    if (m_layerTreeHost)
        m_layerTreeHost->setNonCompositedContentsNeedDisplay(rect);
#endif
    scheduleFrame();
}

// The frame bitmap survives between frames, so what a scroll kept on screen can
// be moved inside it instead of being painted again. Only the strip that came
// into view is then dirty. SUMMIT_SCROLL_COPY=0 goes back to repainting it all.
//
// Moving pixels assumes the previous frame, shifted by a whole number of device
// rows, is still what the page would paint. That holds for app_server, which
// lays glyphs out on whole pixels, and not for Skia, which positions them at
// subpixel offsets: the moved text no longer lands where it was drawn, and over
// a few hundred frames the difference collects into bands of doubled text. So
// it is off by default under Skia, where tiled compositing is the answer
// instead. SUMMIT_SCROLL_COPY=1 turns it back on to look at the artefact.
static bool scrollCopyEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_SCROLL_COPY");
        if (value)
            return !!strcmp(value, "0");
#if USE(HAIKU_GRAPHICS)
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

void DrawingAreaHaiku::scroll(const IntRect& scrollRect, const IntSize& scrollDelta)
{
#if USE(TEXTURE_MAPPER)
    // Composited content scrolls by moving layers; its tiles are kept anyway.
    if (m_layerTreeHost) {
        setNeedsDisplayInRect(scrollRect);
        return;
    }
#endif
    // Only whole rows are moved, so a horizontal scroll is painted as before.
    if (!scrollCopyEnabledHaiku() || scrollRect.isEmpty() || scrollDelta.isZero() || scrollDelta.width()) {
        setNeedsDisplayInRect(scrollRect);
        return;
    }
    if (!m_scrollRect.isEmpty() && scrollRect != m_scrollRect) {
        // Two different areas cannot both be moved in one frame. Keep the
        // larger one and repaint the other (as DrawingAreaCoordinatedGraphics).
        if (unsigned(m_scrollRect.width()) * m_scrollRect.height() >= unsigned(scrollRect.width()) * scrollRect.height()) {
            setNeedsDisplayInRect(scrollRect);
            return;
        }
        setNeedsDisplayInRect(m_scrollRect);
        m_scrollRect = { };
        m_scrollOffset = { };
    }
    // Invalidations already recorded inside the area move with it.
    auto dirtyInScrollRect = intersect(scrollRect, m_dirtyRegion);
    if (!dirtyInScrollRect.isEmpty()) {
        m_dirtyRegion.subtract(scrollRect);
        m_dirtyRegion.unite(intersect(translate(dirtyInScrollRect, scrollDelta), scrollRect));
    }
    // What the move leaves uncovered has to be painted.
    m_dirtyRegion.unite(subtract(scrollRect, translate(scrollRect, scrollDelta)));
    m_scrollRect = scrollRect;
    m_scrollOffset += scrollDelta;
    scheduleFrame();
}

bool DrawingAreaHaiku::copyScrolledPixels(ShareableBitmap& bitmap, float scale,
    const IntRect& scrollRect, const IntSize& scrollOffset)
{
    const int deviceScale = static_cast<int>(std::lround(scale));
    if (deviceScale < 1 || scale != static_cast<float>(deviceScale))
        return false;
    IntRect area = scrollRect;
    area.scale(deviceScale);
    area.intersect(IntRect({ }, bitmap.size()));
    const int delta = scrollOffset.height() * deviceScale;
    if (area.isEmpty() || !delta || std::abs(delta) >= area.height())
        return false;

#if USE(HAIKU_GRAPHICS)
    // app_server draws into this memory from its own team; make sure the
    // previous frame's drawing has landed before moving rows underneath it.
    // Skia rasterises on this thread, so its pixels are already there.
    if (m_frameContext)
        m_frameContext->platformContext()->Sync();
#endif

    auto pixels = bitmap.mutableSpan();
    const size_t stride = bitmap.bytesPerRow();
    const size_t offset = static_cast<size_t>(area.x()) * 4;
    const size_t bytes = static_cast<size_t>(area.width()) * 4;
    if (pixels.size() < stride * static_cast<size_t>(area.maxY()))
        return false;
    auto* base = pixels.data();
    if (delta > 0) {
        for (int y = area.maxY() - 1; y >= area.y() + delta; --y)
            memmove(base + y * stride + offset, base + (y - delta) * stride + offset, bytes);
    } else {
        for (int y = area.y(); y < area.maxY() + delta; ++y)
            memmove(base + y * stride + offset, base + (y - delta) * stride + offset, bytes);
    }
    return true;
}

void DrawingAreaHaiku::triggerRenderingUpdate()
{
    scheduleFrame();
}

bool DrawingAreaHaiku::scheduleRenderingUpdate()
{
    scheduleFrame();
    return true;
}

void DrawingAreaHaiku::updateRenderingWithForcedRepaint()
{
    m_needsDisplay = true;
    m_needsFullRepaint = true;
    if (!m_pendingFrame && !m_isDisplaying)
        display();
}

void DrawingAreaHaiku::updateRenderingWithForcedRepaintAsync(WebPage&, CompletionHandler<void()>&& completionHandler)
{
    m_repaintCallbacks.append(WTF::move(completionHandler));
    updateRenderingWithForcedRepaint();
}

void DrawingAreaHaiku::setRootCompositingLayer(Frame&, GraphicsLayer* graphicsLayer)
{
#if USE(TEXTURE_MAPPER)
    if (m_layerTreeHost)
        m_layerTreeHost->setRootCompositingLayer(graphicsLayer);
#else
    UNUSED_PARAM(graphicsLayer);
#endif
    scheduleFrame();
}

void DrawingAreaHaiku::setLayerTreeStateIsFrozen(bool frozen)
{
    m_isFrozen = frozen;
    if (frozen)
        m_displayTimer.stop();
    else
        scheduleFrame();
}

void DrawingAreaHaiku::activityStateDidChange(OptionSet<ActivityState>, ActivityStateChangeID, CompletionHandler<void()>&& completionHandler)
{
    m_isVisible = m_webPage->corePage()->isVisible();
    if (m_isVisible)
        scheduleFrame();
    else
        m_displayTimer.stop();
    completionHandler();
}

#if USE(TEXTURE_MAPPER)
void DrawingAreaHaiku::updateGeometry(const IntSize& size, CompletionHandler<void()>&& completionHandler)
{
    // The Haiku UI process sends UpdateGeometryHaiku instead. Keep the generic
    // message working with the current geometry identifier and scale.
    updateGeometryHaiku(m_geometryIdentifier, size, m_webPage->corePage()->deviceScaleFactor(), WTF::move(completionHandler));
}

void DrawingAreaHaiku::attachViewOverlayGraphicsLayer(FrameIdentifier, GraphicsLayer* graphicsLayer)
{
    if (m_layerTreeHost)
        m_layerTreeHost->setViewOverlayRootLayer(graphicsLayer);
    scheduleFrame();
}

// WebPage::updatePreferences() calls this after it has applied the store sent
// by the UI process, and it does so again for every later preference change.
// The Haiku UI process always sends acceleratedCompositingEnabled = false
// (UIProcess/haiku/WebView.cpp), because it cannot know whether this process
// has a working GPU. Only this process can, so the decision is applied here,
// every time, or the next unrelated preference change would silently undo it.
void DrawingAreaHaiku::updatePreferences(const WebPreferencesStore&)
{
    applyGLCompositingSettings();
}

void DrawingAreaHaiku::applyGLCompositingSettings()
{
    if (!m_layerTreeHost && LayerTreeHostHaiku::isAvailable()) {
        m_layerTreeHost = LayerTreeHostHaiku::create(m_webPage.get());
        if (!m_layerTreeHost)
            LayerTreeHostHaiku::disableForProcess("the compositor could not be created");
    } else if (m_layerTreeHost && !LayerTreeHostHaiku::isAvailable())
        m_layerTreeHost = nullptr; // Another page hit a GL failure.

    // Compositing is forced, not merely allowed, so that every page of this
    // process takes the same path instead of switching between software and GL
    // frames as composited layers come and go.
    const bool useGL = !!m_layerTreeHost;
    Settings& settings = m_webPage->corePage()->settings();
    settings.setAcceleratedCompositingEnabled(useGL);
    settings.setForceCompositingMode(useGL);
}

void DrawingAreaHaiku::exitGLCompositing(const char* reason)
{
    LayerTreeHostHaiku::disableForProcess(reason);
    // Turning the settings off makes the next rendering update tear the layer
    // tree down, after which the page paints completely in software again.
    applyGLCompositingSettings();
    ASSERT(!m_layerTreeHost);
}
#endif // USE(TEXTURE_MAPPER)

void DrawingAreaHaiku::updateGeometryHaiku(uint64_t geometryIdentifier, const IntSize& size, float scale, CompletionHandler<void()>&& completionHandler)
{
    if ((!size.isEmpty() && !bitmapFrameSizeHaiku(size, scale)) || !std::isfinite(scale) || scale <= 0 || scale > 8) {
        completionHandler();
        return;
    }
    m_geometryIdentifier = geometryIdentifier;
    Ref page = m_webPage.get();
    page->setDeviceScaleFactor(scale);
    page->setSize(size);
#if USE(TEXTURE_MAPPER)
    if (m_layerTreeHost)
        m_layerTreeHost->sizeDidChange();
#endif
    setNeedsDisplay();
    completionHandler();
}

void DrawingAreaHaiku::setDeviceScaleFactor(float scale, CompletionHandler<void()>&& completionHandler)
{
    updateGeometryHaiku(m_geometryIdentifier, m_webPage->size(), scale, WTF::move(completionHandler));
}

void DrawingAreaHaiku::didPresentBitmapHaiku(uint64_t frameIdentifier)
{
    if (m_pendingFrame != frameIdentifier)
        return;
    m_pendingFrame.reset();
    if (m_statistics && m_statistics->sentAt) {
        m_statistics->present += MonotonicTime::now() - m_statistics->sentAt;
        ++m_statistics->presented;
        m_statistics->sentAt = MonotonicTime();
    }
    scheduleDisplay();
}

void DrawingAreaHaiku::scheduleDisplay()
{
    if (!m_needsDisplay || m_pendingFrame || m_isFrozen || m_isDisplaying || (!m_isVisible && m_repaintCallbacks.isEmpty()) || m_displayTimer.isActive())
        return;
    // A frame is due one display period after the previous frame *started*. Waiting
    // a fixed period after the UI process acknowledged the last frame instead added
    // painting and presentation to every interval, which held a page that paints in
    // 10 ms to about 30 frames per second however much headroom the machine had.
    // SUMMIT_FRAME_PACING=ack restores that older behaviour for comparisons.
    static const bool pacesFromAcknowledgement = [] {
        const char* value = getenv("SUMMIT_FRAME_PACING");
        return value && !strcmp(value, "ack");
    }();
    auto period = displayPeriodHaiku();
    if (pacesFromAcknowledgement) {
        m_displayTimer.startOneShot(period);
        return;
    }
    auto elapsed = MonotonicTime::now() - m_lastFrameStart;
    m_displayTimer.startOneShot(elapsed >= period ? 0_s : period - elapsed);
}

void DrawingAreaHaiku::recordFrameStatistics(bool painted, bool unchanged, bool dropped, MonotonicTime frameStart,
    MonotonicTime afterUpdate, MonotonicTime afterDraw, MonotonicTime afterPaint, uint64_t dirtyPixels)
{
    auto& statistics = *m_statistics;
    ++statistics.frames;
    statistics.unchanged += unchanged;
    statistics.dropped += dropped;
    statistics.update += afterUpdate - frameStart;
    if (painted) {
        // "paint" is this process walking the page and sending drawing commands;
        // "flush" is waiting for app_server to finish carrying them out.
        statistics.paint += afterDraw - afterUpdate;
        statistics.flush += afterPaint - afterDraw;
        statistics.dirtyPixels += dirtyPixels;
    }
    if (statistics.previousStart) {
        auto interval = frameStart - statistics.previousStart;
        statistics.interval += interval;
        statistics.longestInterval = std::max(statistics.longestInterval, interval);
    }
    statistics.longestFrame = std::max(statistics.longestFrame, afterPaint - frameStart);
    statistics.previousStart = frameStart;

    auto& painting = WebCore::paintStatisticsHaiku();
    statistics.glyphRuns += painting.glyphRuns;
    statistics.glyphs += painting.glyphs;
    statistics.images += painting.images;
    statistics.fills += painting.fills;
    statistics.strokes += painting.strokes;
    statistics.clips += painting.clips;
    statistics.stateChanges += painting.stateChanges;
    statistics.roundTrips += painting.roundTrips;
    statistics.clipQueries += painting.clipQueries;
    statistics.roundTripTime += painting.roundTripTime;
    statistics.glyphTime += painting.glyphTime;
    statistics.imageTime += painting.imageTime;
    statistics.stateTime += painting.stateTime;
    statistics.fillTime += painting.fillTime;
    statistics.clipTime += painting.clipTime;
    statistics.saveTime += painting.saveTime;
    statistics.primitiveTime += painting.primitiveTime;
    painting.clear();

    auto elapsed = MonotonicTime::now() - statistics.periodStart;
    if (elapsed < statistics.period)
        return;
    auto per = [&](Seconds total, unsigned count) { return count ? total.milliseconds() / count : 0.0; };
#if USE(TEXTURE_MAPPER)
    const char* path = m_layerTreeHost ? " [gl]" : "";
#else
    const char* path = "";
#endif
    WTFLogAlways("Summit frames: %.1f/s over %.1f s (painted=%u unchanged=%u dropped=%u) "
        "update=%.2f ms paint=%.2f ms flush=%.2f ms present=%.2f ms interval=%.2f ms (max %.2f) "
        "longest frame=%.2f ms dirty=%.2f Mpx/frame%s",
        statistics.frames / elapsed.seconds(), elapsed.seconds(),
        statistics.frames - statistics.unchanged - statistics.dropped, statistics.unchanged, statistics.dropped,
        per(statistics.update, statistics.frames),
        per(statistics.paint, statistics.presented ? statistics.presented : statistics.frames),
        per(statistics.flush, statistics.presented ? statistics.presented : statistics.frames),
        per(statistics.present, statistics.presented),
        per(statistics.interval, statistics.frames > 1 ? statistics.frames - 1 : 1),
        statistics.longestInterval.milliseconds(), statistics.longestFrame.milliseconds(),
        statistics.presented ? statistics.dirtyPixels / (1e6 * statistics.presented) : 0.0,
        path);
    // Second line: what those frames asked the port to draw, per frame. Only
    // GraphicsContextHaiku keeps this tally, so with Skia there is nothing to
    // report rather than a line of zeroes.
    auto perFrame = [&](double total) { return statistics.frames ? total / statistics.frames : 0.0; };
#if USE(HAIKU_GRAPHICS)
    WTFLogAlways("Summit painting: per frame drawing calls %.2f ms of which %.0f text runs (%.0f glyphs) %.2f ms, "
        "%.0f images %.2f ms; %.0f fills %.2f ms, %.0f strokes, %.0f clips %.2f ms, "
        "%.0f state changes %.2f ms (%.2f ms in save/restore), "
        "%.0f app_server round trips %.2f ms (%.0f for the clip)",
        perFrame(statistics.primitiveTime.milliseconds()),
        perFrame(statistics.glyphRuns), perFrame(statistics.glyphs), perFrame(statistics.glyphTime.milliseconds()),
        perFrame(statistics.images), perFrame(statistics.imageTime.milliseconds()),
        perFrame(statistics.fills), perFrame(statistics.fillTime.milliseconds()),
        perFrame(statistics.strokes),
        perFrame(statistics.clips), perFrame(statistics.clipTime.milliseconds()),
        perFrame(statistics.stateChanges), perFrame(statistics.stateTime.milliseconds()),
        perFrame(statistics.saveTime.milliseconds()), perFrame(statistics.roundTrips),
        perFrame(statistics.roundTripTime.milliseconds()), perFrame(statistics.clipQueries));
#else
    UNUSED_PARAM(perFrame);
#endif
    auto period = statistics.period;
    auto previousStart = statistics.previousStart;
    statistics = FrameStatistics();
    statistics.period = period;
    statistics.previousStart = previousStart;
    statistics.periodStart = MonotonicTime::now();
}

void DrawingAreaHaiku::completeRepaintCallbacks()
{
    auto callbacks = std::exchange(m_repaintCallbacks, { });
    for (auto& callback : callbacks)
        callback();
}

GraphicsContext* DrawingAreaHaiku::frameContext(ShareableBitmap& bitmap, float scale)
{
    if (!m_frameContext) {
        m_frameContext = bitmap.createGraphicsContext();
        if (m_frameContext)
            m_frameContext->applyDeviceScaleFactor(scale);
    }
    return m_frameContext.get();
}

void DrawingAreaHaiku::display()
{
    if (m_isDisplaying || m_pendingFrame)
        return;
    m_displayTimer.stop();
    if (m_isFrozen || (!m_isVisible && m_repaintCallbacks.isEmpty())) {
        completeRepaintCallbacks();
        return;
    }
    Ref protectedThis { *this };
    auto frameStart = MonotonicTime::now();
    m_lastFrameStart = frameStart;
    uint64_t dirtyPixels = 0;
    {
        SetForScope displaying(m_isDisplaying, true);
        Ref page = m_webPage.get();
        // Clear before layout, which can request another rendering update.
        m_needsDisplay = false;
        if (page->isClosed()) {
            completeRepaintCallbacks();
            return;
        }
        page->updateRendering();
        page->finalizeRenderingUpdate({ });
        page->flushPendingEditorStateUpdate();
        auto afterUpdate = MonotonicTime::now();
        auto afterDraw = afterUpdate;

        float scale = page->corePage()->deviceScaleFactor();
        auto bitmapSize = bitmapFrameSizeHaiku(page->size(), scale);
        if (!bitmapSize) {
            m_frameContext = nullptr;
            m_frameBitmap = nullptr;
        } else if (!m_frameBitmap || m_frameBitmap->size() != *bitmapSize || m_frameBitmapScale != scale) {
            m_frameContext = nullptr;
            m_frameBitmap = ShareableBitmap::create({ *bitmapSize });
            m_frameBitmapScale = scale;
            m_needsFullRepaint = true;
        }
        RefPtr bitmap = m_frameBitmap;

        enum class FrameResult : uint8_t { NotPainted, Painted, Dropped, Unchanged };
        auto result = FrameResult::NotPainted;
#if USE(TEXTURE_MAPPER)
        if (bitmap && m_layerTreeHost) {
            if (m_layerTreeHost->renderFrame(*bitmap)) {
                result = FrameResult::Painted;
                afterDraw = MonotonicTime::now();
                dirtyPixels = static_cast<uint64_t>(page->size().width()) * page->size().height();
                // A later software frame cannot build on pixels composited by GL.
                m_needsFullRepaint = true;
                m_dirtyRegion = { };
                m_scrollRect = { };
                m_scrollOffset = { };
                // Animations run in the compositor and invalidate nothing.
                if (m_layerTreeHost->hasRunningAnimations())
                    m_needsDisplay = true;
            } else {
                // The layer tree still exists, so painting this frame in
                // software would leave out all composited content. Drop the
                // frame instead. Turning the settings off makes the next
                // rendering update tear the layers down, and the frame after
                // that is complete. Repaint callbacks stay queued for it.
                exitGLCompositing("rendering a frame failed");
                m_needsDisplay = true;
                result = FrameResult::Dropped;
            }
        }
#endif
        if (bitmap && result == FrameResult::NotPainted) {
            // Invalidations made by the rendering update above belong to this frame.
            // Diagnostic escape hatch: SUMMIT_FULL_REPAINT=1 repaints the whole page every frame.
            static const bool alwaysRepaintEverything = [] {
                const char* value = getenv("SUMMIT_FULL_REPAINT");
                return value && !strcmp(value, "1");
            }();
            bool fullRepaint = std::exchange(m_needsFullRepaint, false) || alwaysRepaintEverything;
            auto dirtyRegion = std::exchange(m_dirtyRegion, { });
            auto scrollRect = std::exchange(m_scrollRect, { });
            auto scrollOffset = std::exchange(m_scrollOffset, { });
            // Move what the scroll kept on screen; the strip it uncovered is
            // already in the dirty region. If the pixels cannot be moved, the
            // whole scrolled area is painted instead.
            if (!fullRepaint && !scrollRect.isEmpty() && !scrollOffset.isZero()
                && !copyScrolledPixels(*bitmap, scale, scrollRect, scrollOffset))
                dirtyRegion.unite(scrollRect);
            Vector<IntRect, 8> rects;
            if (fullRepaint)
                rects.append(page->bounds());
            else if (!dirtyRegion.isEmpty()) {
                // Many small rectangles cost more app_server traffic than their bounds.
                auto regionRects = dirtyRegion.rects();
                if (regionRects.size() > 8)
                    rects.append(dirtyRegion.bounds());
                else
                    rects.appendVector(regionRects);
            }
            if (rects.isEmpty())
                result = FrameResult::Unchanged;
            else if (auto* context = frameContext(*bitmap, scale)) {
#if USE(HAIKU_GRAPHICS)
                // BView flushes its link to app_server on every PopState(), and
                // WebKit saves and restores state hundreds of times a frame. A
                // view transaction holds those flushes back until the frame is
                // painted, so the commands travel in whole buffers instead of
                // one port write each. SUMMIT_BATCH_DRAWING=0 turns it off.
                // Skia issues no commands to app_server, so it needs none of it.
                static const bool batchedDrawing = [] {
                    const char* value = getenv("SUMMIT_BATCH_DRAWING");
                    return !value || strcmp(value, "0");
                }();
                BWindow* batchWindow = batchedDrawing ? context->platformContext()->Window() : nullptr;
                if (batchWindow)
                    batchWindow->BeginViewTransaction();
#endif
                // No explicit color means the default opaque base background. Only a page that
                // was given a translucent one can show the previous frame through its repaint.
                bool clearsFirst = page->backgroundColor() && !page->backgroundColor()->isOpaque();
                // SUMMIT_DEBUG_DAMAGE=1 paints the damage instead of the page,
                // so a screenshot shows exactly which pixels a frame claimed.
                static const bool showDamage = [] {
                    const char* value = getenv("SUMMIT_DEBUG_DAMAGE");
                    return value && !strcmp(value, "1");
                }();
                for (auto& rect : rects) {
                    GraphicsContextStateSaver stateSaver(*context);
                    context->clip(rect);
                    if (clearsFirst)
                        context->clearRect(rect);
                    if (showDamage)
                        context->fillRect(rect, WebCore::Color::magenta);
                    else
                        page->drawRect(*context, rect);
                    dirtyPixels += static_cast<uint64_t>(rect.width()) * rect.height();
                }
                afterDraw = MonotonicTime::now();
#if USE(HAIKU_GRAPHICS)
                // Finish app_server writes before publishing the shared pixels.
                if (batchWindow)
                    batchWindow->EndViewTransaction();
                context->platformContext()->Sync();
#endif
                result = FrameResult::Painted;
            } else
                m_needsFullRepaint = true;
        }
        auto afterPaint = MonotonicTime::now();
        if (result == FrameResult::Painted) {
            if (auto handle = bitmap->createReadOnlyHandle()) {
                auto frameIdentifier = ++m_frameIdentifier;
                if (send(Messages::DrawingAreaProxy::UpdateBitmapHaiku(m_geometryIdentifier, frameIdentifier, page->size(), scale, WTF::move(*handle)))) {
                    m_pendingFrame = frameIdentifier;
                    if (m_statistics)
                        m_statistics->sentAt = afterPaint;
                }
            }
        }
        if (m_statistics) {
            recordFrameStatistics(result == FrameResult::Painted, result == FrameResult::Unchanged,
                result == FrameResult::Dropped, frameStart, afterUpdate, afterDraw, afterPaint, dirtyPixels);
        }
        page->didUpdateRendering();
        if (result != FrameResult::Dropped)
            completeRepaintCallbacks();
    }
    scheduleDisplay();
}

} // namespace WebKit
