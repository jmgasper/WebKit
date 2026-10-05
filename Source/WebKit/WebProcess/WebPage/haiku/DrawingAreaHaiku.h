/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once


#include "DrawingArea.h"
#include <WebCore/GraphicsContext.h>
#include <WebCore/Region.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>

namespace WebKit {

#if USE(TEXTURE_MAPPER)
class LayerTreeHostHaiku;
#endif

// Paints complete frames into shared bitmaps for DrawingAreaProxyHaiku.
//
// A frame is painted in software, or, with USE(TEXTURE_MAPPER) and when
// LayerTreeHostHaiku::isAvailable(), composited with OpenGL ES 2 and read back
// (docs/gpu-compositing.md in the Summit tree). Frame pacing, the bitmap
// format and the messages are the same in both cases, so the UI process is not
// involved in the choice, and a GL failure falls back to software by itself.
class DrawingAreaHaiku final : public DrawingArea {
    WTF_MAKE_TZONE_ALLOCATED(DrawingAreaHaiku);
public:
    static Ref<DrawingAreaHaiku> create(WebPage&, const WebPageCreationParameters&);
    ~DrawingAreaHaiku() override;

private:
    DrawingAreaHaiku(WebPage&, const WebPageCreationParameters&);
    void setNeedsDisplay() override;
    void setNeedsDisplayInRect(const WebCore::IntRect&) override;
    void scroll(const WebCore::IntRect&, const WebCore::IntSize&) override;
    void triggerRenderingUpdate() override;
    bool scheduleRenderingUpdate() override;
    void updateRenderingWithForcedRepaint() override;
    void updateRenderingWithForcedRepaintAsync(WebPage&, CompletionHandler<void()>&&) override;
    void setRootCompositingLayer(WebCore::Frame&, WebCore::GraphicsLayer*) override;
    void setLayerTreeStateIsFrozen(bool) override;
    bool layerTreeStateIsFrozen() const override { return m_isFrozen; }
    void activityStateDidChange(OptionSet<WebCore::ActivityState>, ActivityStateChangeID, CompletionHandler<void()>&&) override;
    void updateGeometryHaiku(uint64_t, const WebCore::IntSize&, float, CompletionHandler<void()>&&) override;
    void didPresentBitmapHaiku(uint64_t) override;
    void setDeviceScaleFactor(float, CompletionHandler<void()>&&) override;
#if USE(TEXTURE_MAPPER)
    // Pure virtual in DrawingArea whenever USE(TEXTURE_MAPPER) is set.
    void updateGeometry(const WebCore::IntSize&, CompletionHandler<void()>&&) override;
    // Always false: the UI process keeps receiving bitmaps and is never told.
    bool enterAcceleratedCompositingModeIfNeeded() override { return false; }
    void updatePreferences(const WebPreferencesStore&) override;
    void attachViewOverlayGraphicsLayer(WebCore::FrameIdentifier, WebCore::GraphicsLayer*) override;
    void applyGLCompositingSettings();
    void exitGLCompositing(const char* reason);
#endif
    // Requests a frame without invalidating any content.
    void scheduleFrame();
    void scheduleDisplay();
    void display();
    void recordFrameStatistics(bool painted, bool unchanged, bool dropped, MonotonicTime frameStart,
        MonotonicTime afterUpdate, MonotonicTime afterDraw, MonotonicTime afterPaint, uint64_t dirtyPixels);
    // Moves the pixels a scroll kept on screen inside the frame bitmap, so only
    // the strip that came into view has to be painted. False means the frame
    // has to be repainted the ordinary way.
    bool copyScrolledPixels(WebCore::ShareableBitmap&, float scale, const WebCore::IntRect&, const WebCore::IntSize&);
    WebCore::GraphicsContext* frameContext(WebCore::ShareableBitmap&, float scale);
    void completeRepaintCallbacks();

#if USE(TEXTURE_MAPPER)
    std::unique_ptr<LayerTreeHostHaiku> m_layerTreeHost;
#endif
    RunLoop::Timer m_displayTimer;
    Vector<CompletionHandler<void()>> m_repaintCallbacks;
    uint64_t m_geometryIdentifier { 0 };
    uint64_t m_frameIdentifier { 0 };
    std::optional<uint64_t> m_pendingFrame;
    // The UI process copies a frame before acknowledging it, and only one frame is in
    // flight, so the same shared bitmap is repainted where content was invalidated.
    RefPtr<WebCore::ShareableBitmap> m_frameBitmap;
    // Creating a drawing context sets up an app_server offscreen window and thread,
    // about ten times the cost of a typical frame, so it lives as long as the bitmap.
    std::unique_ptr<WebCore::GraphicsContext> m_frameContext;
    float m_frameBitmapScale { 0 };
    WebCore::Region m_dirtyRegion;
    // The area the page scrolled and by how much, accumulated since the last
    // frame, in the same coordinates as the dirty region.
    WebCore::IntRect m_scrollRect;
    WebCore::IntSize m_scrollOffset;

    // SUMMIT_FRAME_STATS=<seconds> prints one summary line per period to stderr,
    // which is how frame pacing is measured on a page that cannot be instrumented
    // from JavaScript (docs/performance.md). Off unless the variable is set.
    struct FrameStatistics {
        Seconds period;
        MonotonicTime periodStart;
        MonotonicTime previousStart;
        MonotonicTime sentAt;
        Seconds update;
        Seconds paint;
        Seconds flush;
        Seconds present;
        Seconds interval;
        Seconds longestInterval;
        Seconds longestFrame;
        unsigned frames { 0 };
        unsigned unchanged { 0 };
        unsigned dropped { 0 };
        unsigned presented { 0 };
        uint64_t dirtyPixels { 0 };
        // What the port was asked to draw, from WebCore::paintStatisticsHaiku().
        unsigned glyphRuns { 0 };
        unsigned glyphs { 0 };
        unsigned images { 0 };
        unsigned fills { 0 };
        unsigned strokes { 0 };
        unsigned clips { 0 };
        unsigned stateChanges { 0 };
        unsigned roundTrips { 0 };
        unsigned clipQueries { 0 };
        Seconds roundTripTime;
        Seconds glyphTime;
        Seconds imageTime;
        Seconds stateTime;
        Seconds fillTime;
        Seconds clipTime;
        Seconds saveTime;
        Seconds primitiveTime;
    };
    std::unique_ptr<FrameStatistics> m_statistics;
    // Frame pacing: the next frame is due one display period after the previous
    // one started, not a fixed delay after the UI process acknowledged the last.
    MonotonicTime m_lastFrameStart;
    bool m_needsFullRepaint { true };
    bool m_needsDisplay { true };
    bool m_isVisible { false };
    bool m_isFrozen { false };
    bool m_isDisplaying { false };
};

} // namespace WebKit
