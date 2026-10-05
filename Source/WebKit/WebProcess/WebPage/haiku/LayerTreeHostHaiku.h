/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#if USE(TEXTURE_MAPPER)

#include <WebCore/GraphicsLayerClient.h>
#include <wtf/Forward.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {
class BitmapTexture;
class GraphicsLayer;
class IntRect;
class ShareableBitmap;
class TextureMapper;
}

namespace WebKit {

class WebPage;

// Composites the page's GraphicsLayerTextureMapper tree with OpenGL ES 2 into
// an offscreen texture and reads the frame back into a ShareableBitmap.
//
// This is the pull-model counterpart of LayerTreeHostTextureMapper. That host
// owns a native window, a flush timer and swapBuffers(); Haiku has no
// cross-process GPU surface, and DrawingAreaHaiku already owns frame pacing
// (one frame in flight, acknowledged by DidPresentBitmapHaiku). So here the
// drawing area asks for a frame and sends the bitmap exactly as the software
// path does. The UI process cannot tell the two paths apart.
//
// Everything runs on the main thread with one process-wide GL context.
class LayerTreeHostHaiku final : public WebCore::GraphicsLayerClient {
    WTF_MAKE_TZONE_ALLOCATED(LayerTreeHostHaiku);
public:
    // Whether GL compositing may be used in this process. The probe runs once,
    // on first call, and the answer never changes from false to true:
    //   SUMMIT_DISABLE_GL_COMPOSITING=1  always software (wins over force);
    //   otherwise EGL must initialize, a GLES2 offscreen context must become
    //   current and a framebuffer readback self-test must pass;
    //   software renderers (llvmpipe, softpipe, ...) are rejected unless
    //   SUMMIT_FORCE_GL_COMPOSITING=1.
    static bool isAvailable();

    // Marks GL compositing unusable for the rest of the process lifetime.
    static void disableForProcess(const char* reason);

    // Returns null when GL compositing is unavailable.
    static std::unique_ptr<LayerTreeHostHaiku> create(WebPage&);
    ~LayerTreeHostHaiku();

    void setRootCompositingLayer(WebCore::GraphicsLayer*);
    void setViewOverlayRootLayer(WebCore::GraphicsLayer*);
    void setNonCompositedContentsNeedDisplay(const WebCore::IntRect&);
    void setNonCompositedContentsNeedDisplay();
    void sizeDidChange();

    // Renders one frame into the bitmap: BGRA, top row first, as the software
    // path produces. The caller must have run the page's rendering update,
    // which flushes the document's compositing state. The bitmap size must be
    // the page size scaled by the device scale factor.
    //
    // Returns false when GL failed. The bitmap contents are then undefined and
    // the caller must paint it in software and stop using this host.
    bool renderFrame(WebCore::ShareableBitmap&);

    // Valid after renderFrame(): another frame is needed to advance animations.
    bool hasRunningAnimations() const { return m_hasRunningAnimations; }

private:
    explicit LayerTreeHostHaiku(WebPage&);

    // GraphicsLayerClient
    void paintContents(const WebCore::GraphicsLayer&, WebCore::GraphicsContext&, const WebCore::FloatRect& rectToPaint, OptionSet<WebCore::GraphicsLayerPaintBehavior>) override;
    float deviceScaleFactor() const override;

    bool initialize();
    void updateRootLayerChildren();
    void applyDeviceScaleFactor();
    bool readPixels(WebCore::ShareableBitmap&);

    WebPage& m_webPage;
    RefPtr<WebCore::GraphicsLayer> m_rootLayer;
    WebCore::GraphicsLayer* m_rootCompositingLayer { nullptr };
    WebCore::GraphicsLayer* m_overlayCompositingLayer { nullptr };
    std::unique_ptr<WebCore::TextureMapper> m_textureMapper;
    RefPtr<WebCore::BitmapTexture> m_surface;
    Vector<uint8_t> m_readbackBuffer;
    bool m_hasRunningAnimations { false };
};

} // namespace WebKit

#endif // USE(TEXTURE_MAPPER)
