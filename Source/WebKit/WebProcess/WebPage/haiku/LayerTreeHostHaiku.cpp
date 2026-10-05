/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// The layer bookkeeping follows LayerTreeHostTextureMapper.cpp and the
// offscreen render-and-read-back step follows WCScene.cpp (both upstream).

#include "config.h"
#include "LayerTreeHostHaiku.h"

#if USE(TEXTURE_MAPPER)

#include "WebPage.h"
#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif
#include <GLES2/gl2ext.h>
#include <WebCore/BitmapTexture.h>
#include <WebCore/GLContext.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/GraphicsLayerTextureMapper.h>
#include <WebCore/DocumentView.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/PlatformDisplayHaiku.h>
#include <WebCore/ShareableBitmap.h>
#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperLayer.h>
#include <array>
#include <cstdlib>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/WTFString.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHostHaiku);

namespace {

enum class Availability : uint8_t { Unknown, Available, Unavailable };

Availability s_availability { Availability::Unknown };
bool s_supportsBGRAReadback { false };

// The only GL context of the process. It is made current once and stays
// current on the main thread, for two reasons: GraphicsLayers release their
// textures whenever they die, and BitmapTexturePool's release timer falls back
// to PlatformDisplay::sharedDisplay().sharingGLContext() when no context is
// current, which would create a second, sharing context.
std::unique_ptr<GLContext>& processContext()
{
    static NeverDestroyed<std::unique_ptr<GLContext>> context;
    return context;
}

bool environmentFlagIsSet(const char* name)
{
    const char* value = getenv(name);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // Haiku port
    return value && value[0] && !(value[0] == '0' && !value[1]);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

bool isSoftwareRenderer(const String& renderer)
{
    auto name = renderer.convertToASCIILowercase();
    for (auto token : { "llvmpipe"_s, "softpipe"_s, "software"_s, "swrast"_s, "lavapipe"_s }) {
        if (name.contains(token))
            return true;
    }
    return false;
}

void drainGLErrors()
{
    // Bounded: a lost context may report errors indefinitely.
    for (unsigned i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) { }
}

// A renderer string alone does not show that the driver works. Clear a small
// framebuffer object and read it back, which is what every frame depends on.
bool framebufferReadbackWorks()
{
    constexpr GLsizei size = 4;
    drainGLErrors();

    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    bool works = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (works) {
        // Asymmetric, so that a red/blue swap is detected too.
        std::array<uint8_t, 4> pixel { 0, 0, 0, 0 };
        glViewport(0, 0, size, size);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        works = glGetError() == GL_NO_ERROR && pixel == std::array<uint8_t, 4> { 255, 0, 0, 255 };
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    return works;
}

Availability probe()
{
    const bool forced = environmentFlagIsSet("SUMMIT_FORCE_GL_COMPOSITING");
    if (environmentFlagIsSet("SUMMIT_DISABLE_GL_COMPOSITING")) {
        WTFLogAlways("Summit GL compositing: disabled by SUMMIT_DISABLE_GL_COMPOSITING; rendering in software.");
        return Availability::Unavailable;
    }

    // setSharedDisplay() may only happen once per process. Nothing else on
    // Haiku installs a display, but do not crash if that ever changes.
    if (PlatformDisplay::sharedDisplayIfExists()) {
        WTFLogAlways("Summit GL compositing: a platform display already exists; rendering in software.");
        return Availability::Unavailable;
    }

    auto display = PlatformDisplayHaiku::create();
    if (!display) {
        WTFLogAlways("Summit GL compositing: EGL did not initialize (%s); rendering in software.", GLContext::lastErrorString());
        return Availability::Unavailable;
    }

    // No sharing context: see processContext(). On Haiku this tries a pbuffer
    // surface first and a surfaceless context second.
    auto context = GLContext::create(display->glDisplay(), GLContext::Target::Default);
    if (!context || !context->makeContextCurrent()) {
        WTFLogAlways("Summit GL compositing: no offscreen OpenGL ES 2 context (%s); rendering in software.", GLContext::lastErrorString());
        context = nullptr; // Before the display that owns it.
        return Availability::Unavailable;
    }

    auto glString = [](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return value ? String::fromLatin1(value) : String();
    };
    const String renderer = glString(GL_RENDERER);
    const String version = glString(GL_VERSION);

    const char* rejection = nullptr;
    if (renderer.isEmpty())
        rejection = "the driver reports no renderer";
    else if (isSoftwareRenderer(renderer) && !forced)
        rejection = "software renderer (set SUMMIT_FORCE_GL_COMPOSITING=1 to use it)";
    else if (!framebufferReadbackWorks())
        rejection = "the framebuffer readback self-test failed";

    if (rejection) {
        WTFLogAlways("Summit GL compositing: %s [%s, %s]; rendering in software.", rejection, renderer.utf8().data(), version.utf8().data());
        context = nullptr; // Before the display that owns it.
        return Availability::Unavailable;
    }

    // Only GL_RGBA/GL_UNSIGNED_BYTE readback is guaranteed by OpenGL ES 2.
    s_supportsBGRAReadback = GLContext::isExtensionSupported(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)), "GL_EXT_read_format_bgra");

    WTFLogAlways("Summit GL compositing: enabled%s [%s, %s, %s readback].", forced ? " (forced)" : "", renderer.utf8().data(), version.utf8().data(), s_supportsBGRAReadback ? "BGRA" : "RGBA+swap");

    // Installing the display keeps PlatformDisplay::sharedDisplay(), which
    // asserts when there is none, safe for BitmapTexturePool.
    PlatformDisplay::setSharedDisplay(WTF::move(display));
    processContext() = WTF::move(context);
    return Availability::Available;
}

} // namespace

bool LayerTreeHostHaiku::isAvailable()
{
    ASSERT(isMainThread());
    if (s_availability == Availability::Unknown)
        s_availability = probe();
    return s_availability == Availability::Available;
}

void LayerTreeHostHaiku::disableForProcess(const char* reason)
{
    ASSERT(isMainThread());
    if (s_availability == Availability::Available)
        WTFLogAlways("Summit GL compositing: %s; falling back to software rendering.", reason);
    // The context and display stay alive: layers created so far still own
    // textures, and they are released as those layers are torn down.
    s_availability = Availability::Unavailable;
}

std::unique_ptr<LayerTreeHostHaiku> LayerTreeHostHaiku::create(WebPage& webPage)
{
    if (!isAvailable())
        return nullptr;

    auto host = std::unique_ptr<LayerTreeHostHaiku>(new LayerTreeHostHaiku(webPage));
    if (!host->initialize())
        return nullptr;
    return host;
}

LayerTreeHostHaiku::LayerTreeHostHaiku(WebPage& webPage)
    : m_webPage(webPage)
{
    m_rootLayer = GraphicsLayer::create(nullptr, *this);
    m_rootLayer->setDrawsContent(true);
    m_rootLayer->setContentsOpaque(true);
    // Scale from the top-left corner, so that applyDeviceScaleFactor() needs
    // no translation to compensate for a centered transform origin.
    m_rootLayer->setAnchorPoint({ });
    m_rootLayer->setSize(m_webPage.size());
    m_rootLayer->setNeedsDisplay();
#ifndef NDEBUG
    m_rootLayer->setName(MAKE_STATIC_STRING_IMPL("Root layer"));
#endif
    applyDeviceScaleFactor();
}

LayerTreeHostHaiku::~LayerTreeHostHaiku()
{
    // Textures are released below and need the context.
    if (auto& context = processContext())
        context->makeContextCurrent();

    m_rootLayer->removeAllChildren();
    m_rootLayer = nullptr;
    m_surface = nullptr;
    m_textureMapper = nullptr;
}

bool LayerTreeHostHaiku::initialize()
{
    auto& context = processContext();
    if (!context || !context->makeContextCurrent())
        return false;

    // Requires a current context.
    m_textureMapper = TextureMapper::create();
    return !!m_textureMapper;
}

void LayerTreeHostHaiku::updateRootLayerChildren()
{
    m_rootLayer->removeAllChildren();
    if (m_overlayCompositingLayer)
        m_rootLayer->addChild(*m_overlayCompositingLayer);
    if (m_rootCompositingLayer)
        m_rootLayer->addChild(*m_rootCompositingLayer);
}

void LayerTreeHostHaiku::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayerChildren();
}

void LayerTreeHostHaiku::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayerChildren();
}

void LayerTreeHostHaiku::setNonCompositedContentsNeedDisplay(const IntRect& rect)
{
    m_rootLayer->setNeedsDisplayInRect(rect);
}

void LayerTreeHostHaiku::setNonCompositedContentsNeedDisplay()
{
    m_rootLayer->setNeedsDisplay();
}

void LayerTreeHostHaiku::sizeDidChange()
{
    // The device scale factor can change without the size changing.
    applyDeviceScaleFactor();

    const auto& newSize = m_webPage.size();
    if (m_rootLayer->size() == newSize)
        return;

    m_rootLayer->setSize(newSize);
    m_rootLayer->setNeedsDisplay();
}

void LayerTreeHostHaiku::applyDeviceScaleFactor()
{
    // Layer geometry is in CSS pixels; backing stores and the target surface
    // are in device pixels.
    TransformationMatrix transform;
    transform.scale(deviceScaleFactor());
    m_rootLayer->setTransform(transform);
}

bool LayerTreeHostHaiku::renderFrame(ShareableBitmap& bitmap)
{
    ASSERT(isMainThread());

    auto& context = processContext();
    if (!isAvailable() || !context || !m_textureMapper || !context->makeContextCurrent())
        return false;

    const IntSize surfaceSize = bitmap.size();
    if (surfaceSize.isEmpty())
        return false;

    const auto maxTextureSize = m_textureMapper->maxTextureSize();
    if (surfaceSize.width() > maxTextureSize.width() || surfaceSize.height() > maxTextureSize.height())
        return false;

    if (!m_surface || m_surface->size() != surfaceSize)
        m_surface = BitmapTexture::create(surfaceSize, { BitmapTexture::Flags::SupportsAlpha });

    drainGLErrors();

    // Page::finalizeRenderingUpdate() has flushed the document's layers. The
    // root and overlay layers are outside of that tree.
    m_rootLayer->flushCompositingStateForThisLayerOnly();
    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(FloatRect({ }, m_rootLayer->size()));

    // Paints dirty tiles with the Haiku GraphicsContext and uploads them.
    auto& rootLayer = downcast<GraphicsLayerTextureMapper>(*m_rootLayer);
    rootLayer.updateBackingStoreIncludingSubLayers(*m_textureMapper);

    m_hasRunningAnimations = rootLayer.layer().applyAnimationsRecursively(MonotonicTime::now());

    // With a BitmapTexture as the surface TextureMapper always flips the Y
    // axis (the FlipY argument only applies to the default framebuffer), so
    // the first row that glReadPixels() returns is the top row of the page,
    // which is also the first row of a BBitmap.
    m_textureMapper->beginPainting(TextureMapper::FlipY::No, m_surface.get());

    bool rendered = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (rendered) {
        // Tile-based GPUs would otherwise reload the previous frame. It also
        // keeps any area that the opaque root layer does not cover opaque.
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        rootLayer.layer().paint(*m_textureMapper);

        // Before endPainting(), which binds the previous framebuffer again.
        rendered = readPixels(bitmap);
    }

    m_textureMapper->endPainting();
    return rendered;
}

bool LayerTreeHostHaiku::readPixels(ShareableBitmap& bitmap)
{
    const size_t width = bitmap.size().width();
    const size_t height = bitmap.size().height();
    const size_t packedBytesPerRow = width * 4;
    const size_t bitmapBytesPerRow = bitmap.bytesPerRow();
    auto destination = bitmap.mutableSpan();
    if (bitmapBytesPerRow < packedBytesPerRow || destination.size() < bitmapBytesPerRow * (height - 1) + packedBytesPerRow)
        return false;

    // glReadPixels() writes rows of exactly width * 4 bytes (the default
    // GL_PACK_ALIGNMENT of 4 never pads them). Use a staging buffer only if
    // the bitmap rows are padded.
    const bool readsDirectly = bitmapBytesPerRow == packedBytesPerRow;
    if (!readsDirectly) {
        m_readbackBuffer.shrink(0);
        if (!m_readbackBuffer.tryGrow(packedBytesPerRow * height))
            return false;
    }

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // Haiku port
    uint8_t* pixels = readsDirectly ? destination.data() : m_readbackBuffer.mutableSpan().data();

    glReadPixels(0, 0, width, height, s_supportsBGRAReadback ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR)
        return false;

    if (!s_supportsBGRAReadback) {
        for (size_t i = 0; i < width * height; ++i)
            std::swap(pixels[i * 4], pixels[i * 4 + 2]);
    }

    if (!readsDirectly) {
        for (size_t y = 0; y < height; ++y)
            memcpy(destination.data() + y * bitmapBytesPerRow, pixels + y * packedBytesPerRow, packedBytesPerRow);
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    return true;
}

void LayerTreeHostHaiku::paintContents(const GraphicsLayer&, GraphicsContext& context, const FloatRect& rectToPaint, OptionSet<GraphicsLayerPaintBehavior>)
{
    context.save();
    context.clip(rectToPaint);
    if (auto* localMainFrame = dynamicDowncast<LocalFrame>(m_webPage.corePage()->mainFrame())) {
        if (auto* view = localMainFrame->view())
            view->paint(context, enclosingIntRect(rectToPaint));
    }
    context.restore();
}

float LayerTreeHostHaiku::deviceScaleFactor() const
{
    return m_webPage.corePage()->deviceScaleFactor();
}

} // namespace WebKit

#endif // USE(TEXTURE_MAPPER)
