/*
 * Copyright (C) 2023 Igalia S.L.
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
#include "AcceleratedSurface.h"

#if PLATFORM(HAIKU)
#include "DirectPresentHaiku.h"
#include "PresentStatsHaiku.h"
#include <image.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#endif

#if USE(COORDINATED_GRAPHICS)
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/BitmapTexture.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/GLFence.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/Region.h>
#include <WebCore/Settings.h>
#include <WebCore/ShareableBitmap.h>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#if PLATFORM(HAIKU)
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/GrTypes.h>
#endif
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#if USE(GBM) || OS(ANDROID)
#include <WebCore/DMABufBuffer.h>
#endif
#include <epoxy/egl.h>
#endif

#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif

#if USE(GBM)
#include <WebCore/DRMDeviceManager.h>
#include <WebCore/GBMVersioning.h>
#include <drm_fourcc.h>
#endif

#if USE(WPE_RENDERER)
#include <WebCore/PlatformDisplayLibWPE.h>
#include <wpe/wpe-egl.h>
#include <wtf/UniStdExtras.h>
#endif

#if OS(ANDROID)
#include <WebCore/BufferFormatAndroid.h>
#include <android/hardware_buffer.h>
#include <drm/drm_fourcc.h>
#include <wtf/android/RefPtrAndroid.h>
#endif

#if USE(GLIB_EVENT_LOOP)
#include <wtf/glib/RunLoopSourcePriority.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedSurface);

#if PLATFORM(HAIKU)
// SUMMIT_SWAPCHAIN_TRACE=1 logs every buffer the swap chains make, lend,
// get back and free, to find buffers that are never returned.
static bool swapChainTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_SWAPCHAIN_TRACE");
        return value && !strcmp(value, "1");
    }();
    return enabled;
}
#endif

Ref<AcceleratedSurface> AcceleratedSurface::create(WebPage& webPage, Function<void()>&& frameCompleteHandler, RenderingPurpose renderingPurpose)
{
    return adoptRef(*new AcceleratedSurface(webPage, WTF::move(frameCompleteHandler), renderingPurpose));
}

static uint64_t generateID()
{
    static uint64_t identifier = 0;
    return ++identifier;
}

static bool useExplicitSync()
{
    auto& display = PlatformDisplay::sharedDisplay();
    auto& extensions = display.eglExtensions();
    return extensions.ANDROID_native_fence_sync && (display.eglCheckVersion(1, 5) || extensions.KHR_fence_sync);
}

AcceleratedSurface::AcceleratedSurface(WebPage& webPage, Function<void()>&& frameCompleteHandler, RenderingPurpose renderingPurpose)
    : m_webPage(webPage)
    , m_frameCompleteHandler(WTF::move(frameCompleteHandler))
#if USE(TEXTURE_MAPPER)
    , m_useSkia(renderingPurpose == RenderingPurpose::NonComposited)
#else
    , m_useSkia(true)
#endif
    , m_id(generateID())
    , m_renderingPurpose(renderingPurpose)
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
    , m_hardwareAccelerationEnabled(webPage.corePage()->settings().hardwareAccelerationEnabled())
#endif
    , m_backgroundColor(webPage.backgroundColor())
    , m_swapChain(*this)
    , m_isVisible(webPage.activityState().contains(ActivityState::IsVisible))
    , m_useExplicitSync(usesGL() && useExplicitSync())
#if ENABLE(DAMAGE_TRACKING)
#if PLATFORM(HAIKU)
    // The readback walks the rectangles (see readBackThroughPixelBuffer); a
    // bounding box made a scrollbar and an ad at opposite sides the whole frame.
    , m_damageTracker(m_swapChain, [] {
        // SUMMIT_FINE_DAMAGE=0 keeps one bounding box per target, as before.
        const char* value = getenv("SUMMIT_FINE_DAMAGE");
        return !value || strcmp(value, "0");
    }())
#else
    , m_damageTracker(m_swapChain, !usesGL())
#endif
#endif
{
}

#if PLATFORM(HAIKU)
// app_server's copy of the screen, mapped once for the life of the process.
// Mesa keeps the pages of the mapping it presents into locked for the GPU,
// and deleting an area whose pages are locked can hang the system; a page
// that changes size gets a new surface (and presenter), so the mapping must
// not belong to a presenter.
static void* backBufferMappingHaiku(area_id source)
{
    static NeverDestroyed<std::mutex> lock;
    static NeverDestroyed<HashMap<area_id, void*>> mappings;
    if (source < 0)
        return nullptr;
    // Off unless SUMMIT_DIRECT_PRESENT_BACK=1: a web process that ends while
    // the driver holds these pages for the GPU hangs the X399 (Haiku tears the
    // address space down before it closes the driver), and a crash ends a
    // process that way. Until that is fixed in the OS, app_server's copy is
    // kept current by a frame through app_server now and then instead
    // (AcceleratedSurface::didRenderFrame).
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_DIRECT_PRESENT_BACK");
        return value && !strcmp(value, "1");
    }();
    if (!enabled)
        return nullptr;
    std::lock_guard guard(lock.get());
    auto found = mappings->find(source);
    if (found != mappings->end())
        return found->value;
    void* address = nullptr;
    area_id clone = clone_area("direct present back buffer", &address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, source);
    mappings->set(source, clone >= 0 ? address : nullptr);
    return clone >= 0 ? address : nullptr;
}

static bool directPresentTraceHaiku();

// Copies composited frames straight into the screen where the UI process
// says the page is (DirectPresentHaiku.h), through Summit's private Mesa
// (patch 06, summit_haiku_present_framebuffer in libEGL_mesa). Compositing
// thread, apart from the destructor.
class DirectPresenterHaiku {
    WTF_MAKE_NONCOPYABLE(DirectPresenterHaiku);
public:
    // Whether this process's libEGL_mesa has Summit's present entry points.
    static bool canPresent() { return functions().present != nullptr; }

    static std::unique_ptr<DirectPresenterHaiku> create(area_id source)
    {
        std::unique_ptr<DirectPresenterHaiku> presenter(new DirectPresenterHaiku);
        void* address = nullptr;
        presenter->m_clone = clone_area("direct present target", &address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, source);
        if (presenter->m_clone < 0)
            return nullptr;
        presenter->m_state = static_cast<DirectPresentStateHaiku*>(address);
        if (presenter->m_state->magic != DirectPresentStateHaiku::magicValue)
            return nullptr;
        return presenter;
    }

    ~DirectPresenterHaiku()
    {
        // The GPU's completion callbacks write into the shared state.
        if (m_presentedAny && functions().waitIdle)
            functions().waitIdle();
        // The last presenter of the process lets Mesa give up the frame buffer
        // and app_server's copy while nothing is in flight: the driver tearing
        // those imports down with a copy still running has hung the machine,
        // and a process that ends with them held tears them down that way.
        if (!--liveCount() && functions().release)
            functions().release();
        if (m_clone >= 0)
            delete_area(m_clone);
    }

    bool wanted() const { return m_state->enabled && functions().present; }

    // With the frame's framebuffer object bound for drawing. damage is what
    // changed since the screen last got a frame, in the frame's pixels.
    bool present(unsigned framebuffer, const IntSize& frameSize, const Vector<IntRect, 1>& damage)
    {
        auto present = functions().present;
        if (!present || !m_state->enabled)
            return false;
        // The UI process holds the lock only while it changes the target.
        if (acquire_sem_etc(m_state->lock, 1, B_RELATIVE_TIMEOUT, 20000) != B_OK)
            return false;
        auto release = makeScopeExit([this] { release_sem(m_state->lock); });
        if (!m_state->enabled)
            return false;

        if (m_state->backArea != m_backSource) {
            m_backSource = m_state->backArea;
            m_backBits = backBufferMappingHaiku(m_backSource);
        }

        // A changed place is presented in full: the screen holds nothing of
        // the page in a part that has just become visible. So is a new
        // presenter's first frame: the state is the view's, and the process
        // that presented into it last (before a navigation to another site,
        // say) showed another page.
        bool full = !m_presentedAny || m_state->generation != m_state->presentedGeneration.load();
        uint32_t visibleCount = std::min(m_state->rectCount, DirectPresentStateHaiku::maximumRects);
        m_rects.clear();
        for (uint32_t i = 0; i < visibleCount; ++i) {
            const int32_t* visible = m_state->rects + i * 4;
            if (full) {
                m_rects.append({ visible[0], visible[1], visible[2], visible[3] });
                continue;
            }
            for (auto& rect : damage) {
                int32_t left = std::max(visible[0], m_state->originX + rect.x());
                int32_t top = std::max(visible[1], m_state->originY + rect.y());
                int32_t right = std::min(visible[2], m_state->originX + rect.maxX() - 1);
                int32_t bottom = std::min(visible[3], m_state->originY + rect.maxY() - 1);
                if (left <= right && top <= bottom)
                    m_rects.append({ left, top, right, bottom });
            }
        }
        // Mesa takes 256 rectangles; past that, each visible one whole.
        if (m_rects.size() > 256) {
            m_rects.clear();
            for (uint32_t i = 0; i < visibleCount; ++i) {
                const int32_t* visible = m_state->rects + i * 4;
                m_rects.append({ visible[0], visible[1], visible[2], visible[3] });
            }
        }

        SummitHaikuPresent request { };
        request.version = 1;
        request.originX = m_state->originX;
        request.originY = m_state->originY;
        request.rectCount = m_rects.size();
        request.rects = m_rects.isEmpty() ? nullptr : &m_rects[0][0];
        request.frontBytesPerRow = m_state->frontBytesPerRow;
        if (m_backBits) {
            request.backBits = m_backBits;
            request.backBytesPerRow = m_state->backBytesPerRow;
            request.backWidth = m_state->backWidth;
            request.backHeight = m_state->backHeight;
        }
        request.framesInFlight = 2;
        request.completed = [](void* cookie) { static_cast<std::atomic<int32_t>*>(cookie)->fetch_sub(1); };
        request.cookie = &m_state->inFlight;
        m_state->inFlight.fetch_add(1);
        int result = present(framebuffer, &request);
        // -5: what changed is outside the screen; the frame is presented all the same.
        if (result == -5) {
            m_state->inFlight.fetch_sub(1);
            result = 0;
        } else if (result) {
            m_state->inFlight.fetch_sub(1);
            if (std::exchange(m_lastFailure, result) != result)
                std::fprintf(stderr, "Summit direct present: not presented (%d), reading frames back\n", result);
            return false;
        }
        if (std::exchange(m_lastFailure, 0) || !m_presentedAny)
            std::fprintf(stderr, "Summit direct present: presenting %dx%d frames into the screen at %d,%d\n",
                frameSize.width(), frameSize.height(), m_state->originX, m_state->originY);
        if (full && directPresentTraceHaiku()) {
            double pixels = 0;
            for (auto& rect : m_rects)
                pixels += double(rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1);
            std::fprintf(stderr, "Summit direct present: in full, generation %u, %zu rectangles, %.2f Mpx\n",
                m_state->generation, m_rects.size(), pixels / 1e6);
        }
        m_presentedAny = true;
        m_state->presentedGeneration = m_state->generation;
        m_state->presents.fetch_add(1);
        if (PresentStageStatsHaiku::enabled()) {
            static NeverDestroyed<PresentStageStatsHaiku> copied("pixels presented directly (web)");
            double pixels = 0;
            for (auto& rect : m_rects)
                pixels += double(rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1);
            copied.get().add(0, pixels / 1e6);
        }
        return true;
    }

private:
    DirectPresenterHaiku() { ++liveCount(); }
    // Presenters alive in this process; compositing threads and the main one.
    static std::atomic<int>& liveCount()
    {
        static std::atomic<int> count { 0 };
        return count;
    }

    // Mirrors struct summit_haiku_present in Mesa's zink_haiku_present.h.
    struct SummitHaikuPresent {
        uint32_t version;
        int32_t originX, originY;
        uint32_t rectCount;
        const int32_t* rects;
        uint32_t frontBytesPerRow;
        void* backBits;
        uint32_t backBytesPerRow;
        uint32_t backWidth, backHeight;
        uint32_t framesInFlight;
        void (*completed)(void*);
        void* cookie;
    };
    struct Functions {
        int (*present)(unsigned, const SummitHaikuPresent*) { nullptr };
        void (*waitIdle)() { nullptr };
        void (*release)() { nullptr };
    };
    // Found in the loaded libEGL_mesa: EGL's own dispatch never sees them.
    static const Functions& functions()
    {
        static Functions found = [] {
            Functions result;
            const char* value = std::getenv("SUMMIT_DIRECT_PRESENT");
            if (value && !std::strcmp(value, "0"))
                return result;
            image_info info;
            int32 cookie = 0;
            while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
                if (!std::strstr(info.name, "libEGL_mesa"))
                    continue;
                void* present = nullptr;
                void* waitIdle = nullptr;
                if (get_image_symbol(info.id, "summit_haiku_present_framebuffer", B_SYMBOL_TYPE_TEXT, &present) == B_OK
                    && get_image_symbol(info.id, "summit_haiku_present_wait_idle", B_SYMBOL_TYPE_TEXT, &waitIdle) == B_OK) {
                    void* release = nullptr;
                    // Without a way to let go of the imports, presenting into
                    // app_server's copy at all is not safe (see the destructor).
                    if (get_image_symbol(info.id, "summit_haiku_present_release", B_SYMBOL_TYPE_TEXT, &release) == B_OK) {
                        result.present = reinterpret_cast<int (*)(unsigned, const SummitHaikuPresent*)>(present);
                        result.waitIdle = reinterpret_cast<void (*)()>(waitIdle);
                        result.release = reinterpret_cast<void (*)()>(release);
                    }
                }
                break;
            }
            if (!result.present)
                std::fprintf(stderr, "Summit direct present: libEGL_mesa has no summit_haiku_present_* (an older Mesa); frames are read back\n");
            return result;
        }();
        return found;
    }

    area_id m_clone { -1 };
    DirectPresentStateHaiku* m_state { nullptr };
    area_id m_backSource { -1 };
    void* m_backBits { nullptr };
    Vector<std::array<int32_t, 4>> m_rects;
    int m_lastFailure { 0 };
    bool m_presentedAny { false };
};

static bool directPresentTraceHaiku()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_DIRECT_PRESENT_TRACE");
        return value && !strcmp(value, "1");
    }();
    return enabled;
}

void AcceleratedSurface::setDirectPresentTarget(int32_t area)
{
    // A target this process cannot present into (a Mesa without Summit's
    // present entry points, such as the Raspberry Pi's) is declined with a
    // DirectFrame for target 0, which no render target has. Until then the
    // browser holds every FrameDone until app_server has drawn the frame and
    // offers the target again each second: on the Pi that held scrolling to
    // 7 frames a second.
    if (area >= 0 && !DirectPresenterHaiku::canPresent()) {
        m_directPresenter = nullptr;
        WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DirectFrame(0), surfaceID());
        return;
    }
    m_directPresenter = area >= 0 ? DirectPresenterHaiku::create(area) : nullptr;
    if (directPresentTraceHaiku())
        std::fprintf(stderr, "Summit direct present: surface %llu target area %d: %s, wanted %d\n", static_cast<unsigned long long>(surfaceID()),
            static_cast<int>(area), m_directPresenter ? "presenter" : "none", m_directPresenter && m_directPresenter->wanted());
}

void AcceleratedSurface::redisplayDirect()
{
    if (m_redisplayHandlerHaiku)
        m_redisplayHandlerHaiku();
}
#endif

AcceleratedSurface::~AcceleratedSurface() = default;

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedSurface::RenderTarget);

static uint64_t generateTargetID()
{
    static uint64_t identifier = 0;
    return ++identifier;
}

AcceleratedSurface::RenderTarget::RenderTarget(AcceleratedSurface& surface, const IntSize& size)
    : m_id(generateTargetID())
    , m_surface(surface)
    , m_size(size)
{
}

AcceleratedSurface::RenderTarget::~RenderTarget() = default;

void AcceleratedSurface::RenderTarget::createSkiaSurfaceForFramebuffer(unsigned fbo)
{
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("AcceleratedSurface was unable to construct a complete framebuffer for the Skia render target");
        return;
    }

    GLint stencilBits;
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);

    auto& display = PlatformDisplay::sharedDisplay();
    GLContext::ScopedGLContextCurrent scopedCurrent(*display.skiaGLContext());

    GrGLFramebufferInfo framebufferInfo;
    framebufferInfo.fFBOID = fbo;
    framebufferInfo.fFormat = GL_RGBA8;
    auto backendRenderTarget = GrBackendRenderTargets::MakeGL(m_size.width(), m_size.height(), 0, stencilBits, framebufferInfo);
    auto origin = m_surface->shouldPaintMirrored() ? kBottomLeft_GrSurfaceOrigin : kTopLeft_GrSurfaceOrigin;
    auto properties = FontRenderOptions::singleton().createSurfaceProps();
    m_skiaSurface = SkSurfaces::WrapBackendRenderTarget(display.skiaGrContext(), backendRenderTarget, origin, kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
}

#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedSurface::RenderTargetShareableBuffer);

AcceleratedSurface::RenderTargetShareableBuffer::RenderTargetShareableBuffer(AcceleratedSurface& surface, const IntSize& size)
    : RenderTarget(surface, size)
{
    if (!m_surface->usesGL())
        return;

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenRenderbuffers(1, &m_depthStencilBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthStencilBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, size.width(), size.height());

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthStencilBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depthStencilBuffer);
}

AcceleratedSurface::RenderTargetShareableBuffer::~RenderTargetShareableBuffer()
{
    m_skiaSurface = nullptr;

    if (m_fbo)
        glDeleteFramebuffers(1, &m_fbo);

    if (m_depthStencilBuffer)
        glDeleteRenderbuffers(1, &m_depthStencilBuffer);

    if (m_colorBuffer)
        glDeleteRenderbuffers(1, &m_colorBuffer);

    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidDestroyBuffer(m_id), m_surface->surfaceID());
}

void AcceleratedSurface::RenderTargetShareableBuffer::initializeColorBuffer(EGLImage image)
{
    glGenRenderbuffers(1, &m_colorBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_colorBuffer);
    if (image)
        glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
    else {
#if PLATFORM(HAIKU)
        // The frame is read back as BGRA, the order a BBitmap holds. From an
        // RGBA target the driver first converts a copy of the frame on the
        // GPU (1.2 ms of a 5 ms read of 3840x1756 on zink); a BGRA target
        // is copied as it is. SUMMIT_BGRA_TARGET=0 keeps RGBA.
        static const bool bgraTarget = [] {
            const char* value = getenv("SUMMIT_BGRA_TARGET");
            if (value && !strcmp(value, "0"))
                return false;
            auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
            return extensions && strstr(extensions, "GL_EXT_texture_format_BGRA8888") && strstr(extensions, "GL_EXT_read_format_bgra");
        }();
        bool stored = false;
        if (bgraTarget && !m_surface->useSkia()) {
            while (glGetError() != GL_NO_ERROR) { }
            glRenderbufferStorage(GL_RENDERBUFFER, 0x93A1 /* GL_BGRA8_EXT */, m_size.width(), m_size.height());
            stored = glGetError() == GL_NO_ERROR;
        }
        if (!stored)
#endif
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_size.width(), m_size.height());
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorBuffer);
#if PLATFORM(HAIKU)
    if (!image && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_size.width(), m_size.height());
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorBuffer);
    }
#endif

    if (m_surface->useSkia())
        createSkiaSurfaceForFramebuffer(m_fbo);
}

void AcceleratedSurface::RenderTargetShareableBuffer::sendFrame(Vector<WebCore::IntRect, 1>&& damageRects)
{
#if PLATFORM(HAIKU)
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit swapchain: surface=%llu send=%llu\n", static_cast<unsigned long long>(m_surface->surfaceID()), static_cast<unsigned long long>(m_id));
#endif
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::Frame(m_id, WTF::move(damageRects), WTF::move(m_renderingFenceFD)), m_surface->surfaceID());
}

void AcceleratedSurface::RenderTargetShareableBuffer::willRenderFrame()
{
    if (m_releaseFenceFD) {
        if (auto fence = GLFence::importFD(PlatformDisplay::sharedDisplay().glDisplay(), WTF::move(m_releaseFenceFD)))
            fence->serverWait();
    }

    if (!m_surface->useSkia()) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG_ERROR("AcceleratedSurface was unable to construct a complete framebuffer");
    }
}

std::unique_ptr<GLFence> AcceleratedSurface::RenderTargetShareableBuffer::createRenderingFence(bool useExplicitSync) const
{
    auto& display = PlatformDisplay::sharedDisplay().glDisplay();
    if (useExplicitSync && supportsExplicitSync()) {
        if (auto fence = GLFence::createExportable(display))
            return fence;
    }
    return GLFence::create(display);
}

void AcceleratedSurface::RenderTargetShareableBuffer::sync(bool useExplicitSync)
{
    if (auto fence = createRenderingFence(useExplicitSync)) {
        m_renderingFenceFD = fence->exportFD();
        if (!m_renderingFenceFD)
            fence->clientWait();
    } else if (!m_surface->useSkia())
        glFlush();
}

void AcceleratedSurface::RenderTargetShareableBuffer::setReleaseFenceFD(UnixFileDescriptor&& releaseFence)
{
    m_releaseFenceFD = WTF::move(releaseFence);
}

#if USE(GBM)
std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::RenderTargetEGLImage::create(AcceleratedSurface& surface, const IntSize& size, const BufferFormat& bufferFormat)
{
    if (!bufferFormat.fourcc) {
        WTFLogAlways("Failed to create GBM buffer of size %dx%d: no valid format found", size.width(), size.height());
        return nullptr;
    }

    auto gbmDevice = bufferFormat.gbmDevice;
    if (!gbmDevice) {
        gbmDevice = DRMDeviceManager::singleton().mainGBMDevice(bufferFormat.usage == RendererBufferFormat::Usage::Scanout ?
            DRMDeviceManager::NodeType::Primary : DRMDeviceManager::NodeType::Render);
    }
    if (!gbmDevice) {
        WTFLogAlways("Failed to create GBM buffer of size %dx%d: no GBM device found", size.width(), size.height());
        return nullptr;
    }

    struct gbm_bo* bo = nullptr;
    uint32_t flags = bufferFormat.usage == RendererBufferFormat::Usage::Scanout ? GBM_BO_USE_SCANOUT : GBM_BO_USE_RENDERING;
    auto enableModifiers = bufferFormat.modifiers.size() == 1 && bufferFormat.modifiers[0] == DRM_FORMAT_MOD_INVALID
        ? DMABufBufferAttributes::EnableModifiers::No : DMABufBufferAttributes::EnableModifiers::Yes;
    if (enableModifiers == DMABufBufferAttributes::EnableModifiers::Yes && !bufferFormat.modifiers.isEmpty())
        bo = gbm_bo_create_with_modifiers2(gbmDevice->device(), size.width(), size.height(), bufferFormat.fourcc, bufferFormat.modifiers.span().data(), bufferFormat.modifiers.size(), flags);

    if (!bo) {
        if (bufferFormat.usage == RendererBufferFormat::Usage::Mapping)
            flags |= GBM_BO_USE_LINEAR;
        bo = gbm_bo_create(gbmDevice->device(), size.width(), size.height(), bufferFormat.fourcc, flags);
    }

    if (!bo) {
        WTFLogAlways("Failed to create GBM buffer of size %dx%d: %s", size.width(), size.height(), safeStrerror(errno).data());
        return nullptr;
    }

    auto dmaBufAttributes = DMABufBufferAttributes::fromGBMBufferObject(bo, enableModifiers);
    if (!dmaBufAttributes) {
        gbm_bo_destroy(bo);
        WTFLogAlways("Failed to extract DMA-buf attributes from GBM buffer of size %dx%d", size.width(), size.height()); // NOLINT
        return nullptr;
    }

    auto& display = PlatformDisplay::sharedDisplay();
    auto image = DMABufBuffer::createEGLImage(display.glDisplay(), *dmaBufAttributes);
    gbm_bo_destroy(bo);
    if (!image) {
        WTFLogAlways("Failed to create EGL image for DMABufs with size %dx%d", size.width(), size.height());
        return nullptr;
    }

    return makeUnique<RenderTargetEGLImage>(surface, size, image, WTF::move(*dmaBufAttributes), bufferFormat.usage);
}

AcceleratedSurface::RenderTargetEGLImage::RenderTargetEGLImage(AcceleratedSurface& surface, const IntSize& size, EGLImage image, DMABufBufferAttributes&& dmaBufAttributes, RendererBufferFormat::Usage usage)
    : RenderTargetShareableBuffer(surface, size)
    , m_image(image)
{
    initializeColorBuffer(m_image);
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateDMABufBuffer(m_id, WTF::move(dmaBufAttributes), usage), m_surface->surfaceID());
}
#endif // USE(GBM)

#if OS(ANDROID)
static uint64_t usageToAHardwareBufferUsage(RendererBufferFormat::Usage usage)
{
    switch (usage) {
    case RendererBufferFormat::Usage::Rendering:
        return AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    case RendererBufferFormat::Usage::Mapping:
        return AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    case RendererBufferFormat::Usage::Scanout:
        // FIXME(297316): Add the AHARDWAREBUFFER_USAGE_CPU_READ_RARELY flag to allow using AHardwareBuffer_lock()
        return AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_FRONT_BUFFER | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    }
}

std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::RenderTargetEGLImage::create(AcceleratedSurface& surface, const WebCore::IntSize& size, const BufferFormat& bufferFormat)
{
    const auto hardwareBufferFormat = toAHardwareBufferFormat(bufferFormat.fourcc);
    if (!hardwareBufferFormat) {
        LOG_ERROR("Failed to create AHardwareBuffer of size %dx%d: no valid format found (FourCC=%s)",
            size.width(), size.height(), FourCC(bufferFormat.fourcc).string().data());
        return nullptr;
    }

    AHardwareBuffer_Desc description = { };
    description.width = size.width();
    description.height = size.height();
    description.layers = 1;
    description.format = hardwareBufferFormat.value();
    description.usage = usageToAHardwareBufferUsage(bufferFormat.usage);

    AHardwareBuffer* hardwareBufferPtr { nullptr };
    int result = AHardwareBuffer_allocate(&description, &hardwareBufferPtr);
    if (result) {
        LOG_ERROR("Failed to create AHardwareBuffer of size %dx%d, format %s, error code: %d",
            size.width(), size.height(), FourCC(bufferFormat.fourcc).string().data(), result);
        return nullptr;
    }
    auto hardwareBuffer = adoptRef(hardwareBufferPtr);

    const Vector<EGLAttrib> attributes { EGL_IMAGE_PRESERVED, EGL_TRUE, EGL_NONE };

    auto& display = WebCore::PlatformDisplay::sharedDisplay();
    auto clientBuffer = eglGetNativeClientBufferANDROID(hardwareBuffer.get());
    if (!clientBuffer) {
        LOG_ERROR("Failed to create client buffer for AHarwareBuffer of size %dx%d, format %s. EGL error: %#04x",
            size.width(), size.height(), FourCC(bufferFormat.fourcc).string().data(), eglGetError());
        return nullptr;
    }

    auto image = display.createEGLImage(EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attributes);
    if (image == EGL_NO_IMAGE) {
        LOG_ERROR("Failed to bind AHardwareBuffer to an EGLImage. This is typically caused by "
            "a version mismatch between the gralloc implementation and the OpenGL/EGL driver. "
            "Please contact your GPU vendor to resolve this problem. EGL error: %#04x", eglGetError());
        return nullptr;
    }

    return makeUnique<RenderTargetEGLImage>(surface, size, image, WTF::move(hardwareBuffer));
}

AcceleratedSurface::RenderTargetEGLImage::RenderTargetEGLImage(AcceleratedSurface& surface, const WebCore::IntSize& size, EGLImage image, RefPtr<AHardwareBuffer>&& hardwareBuffer)
    : RenderTargetShareableBuffer(surface, size)
    , m_image(image)
{
    initializeColorBuffer(m_image);
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateAndroidBuffer(m_id, WTF::move(hardwareBuffer)), m_surface->surfaceID());
}
#endif // OS(ANDROID)

#if USE(GBM) || OS(ANDROID)
AcceleratedSurface::RenderTargetEGLImage::~RenderTargetEGLImage()
{
    if (m_image)
        PlatformDisplay::sharedDisplay().destroyEGLImage(m_image);
}
#endif // USE(GBM) || OS(ANDROID)

std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::RenderTargetSHMImage::create(AcceleratedSurface& surface, const IntSize& size)
{
    RefPtr buffer = ShareableBitmap::create({ size });
    if (!buffer) {
        WTFLogAlways("Failed to allocate shared memory buffer of size %dx%d", size.width(), size.height());
        return nullptr;
    }

#if PLATFORM(HAIKU)
    // app_server can only draw an area it may clone for writing, so the UI
    // process needs a writable mapping to show this buffer without copying it.
    // It could always overwrite frames it is shown anyway.
    auto bufferHandle = buffer->createHandle(SharedMemory::Protection::ReadWrite);
#else
    auto bufferHandle = buffer->createReadOnlyHandle();
#endif
    if (!bufferHandle) {
        WTFLogAlways("Failed to create handle for shared memory buffer");
        return nullptr;
    }

    return makeUnique<RenderTargetSHMImage>(surface, size, Ref { *buffer }, WTF::move(*bufferHandle));
}

AcceleratedSurface::RenderTargetSHMImage::RenderTargetSHMImage(AcceleratedSurface& surface, const IntSize& size, Ref<ShareableBitmap>&& bitmap, ShareableBitmap::Handle&& bitmapHandle)
    : RenderTargetShareableBuffer(surface, size)
    , m_bitmap(WTF::move(bitmap))
{
    if (m_surface->usesGL())
        initializeColorBuffer();
    else if (m_surface->useSkia())
        m_skiaSurface = m_bitmap->createSurface();

    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateSHMBuffer(m_id, WTF::move(bitmapHandle)), m_surface->surfaceID());
}

#if PLATFORM(HAIKU)
AcceleratedSurface::RenderTargetSHMImage::~RenderTargetSHMImage()
{
    if (m_pixelBuffer)
        glDeleteBuffers(1, &m_pixelBuffer);
}

// The damage comes as grid cells. Cells of one column that touch make one
// strip, strips of one row that touch make one band - both exact. Then pairs
// merge, the one that reads the fewest extra pixels first, while that costs
// less than another read would (each glReadPixels waits for the GPU, about a
// millisecond on zink, as long as copying half a megapixel) or while there
// are more than 16.
static Vector<IntRect, 1> mergedReadbackRectsHaiku(Vector<IntRect, 1>&& rects)
{
    static constexpr size_t maximumReads = 16;
    static constexpr int64_t pixelsWorthOneRead = 500000;
    if (rects.size() <= 1)
        return WTF::move(rects);
    auto mergeTouching = [](Vector<IntRect, 1>& list, bool vertical) {
        std::sort(list.begin(), list.end(), [vertical](const IntRect& a, const IntRect& b) {
            if (vertical)
                return std::tuple(a.x(), a.width(), a.y()) < std::tuple(b.x(), b.width(), b.y());
            return std::tuple(a.y(), a.height(), a.x()) < std::tuple(b.y(), b.height(), b.x());
        });
        Vector<IntRect, 1> merged;
        for (auto& rect : list) {
            if (!merged.isEmpty()) {
                auto& last = merged.last();
                bool sameSpan = vertical ? (last.x() == rect.x() && last.width() == rect.width()) : (last.y() == rect.y() && last.height() == rect.height());
                bool touching = vertical ? rect.y() <= last.maxY() : rect.x() <= last.maxX();
                if (sameSpan && touching) {
                    last.unite(rect);
                    continue;
                }
            }
            merged.append(rect);
        }
        list = WTF::move(merged);
    };
    mergeTouching(rects, true);
    mergeTouching(rects, false);
    // Pairwise merging is cubic; with this many pieces most of the frame
    // changed anyway (a scroll, a busy page), so read their bounds.
    static constexpr size_t maximumToMergeInPairs = 32;
    if (rects.size() > maximumToMergeInPairs) {
        IntRect bounds;
        for (auto& rect : rects)
            bounds.unite(rect);
        return { bounds };
    }
    auto area = [](const IntRect& rect) { return int64_t(rect.width()) * int64_t(rect.height()); };
    while (rects.size() > 1) {
        size_t bestA = 0;
        size_t bestB = 1;
        int64_t bestCost = std::numeric_limits<int64_t>::max();
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                auto united = unionRect(rects[i], rects[j]);
                // Overlapping rects cost less than nothing to merge.
                int64_t cost = area(united) - area(rects[i]) - area(rects[j]);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestA = i;
                    bestB = j;
                }
            }
        }
        if (rects.size() <= maximumReads && bestCost > pixelsWorthOneRead)
            break;
        rects[bestA].unite(rects[bestB]);
        rects.removeAt(bestB);
    }
    return WTF::move(rects);
}

// Rows per glReadPixels() call. Mesa's v3d (Raspberry Pi 4) de-tiles a read
// into a staging buffer the size of the read and converts from there: a whole
// 1920x1000 frame (7.7 MB, far beyond the A72's 1 MB L2) reads at 139 MB/s,
// bands of 16 to 64 rows at 260 MB/s (55 ms against 29). Other drivers pay
// for each read (a quarter of a millisecond on zink), so only V3D reads in
// bands unless SUMMIT_READBACK_BAND_KB says how large a band is (0: whole
// rectangles).
static int readbackBandRowsHaiku(int width)
{
    static const int bandBytes = [] {
        if (const char* value = getenv("SUMMIT_READBACK_BAND_KB"))
            return atoi(value) * 1024;
        auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        return renderer && !strncmp(renderer, "V3D", 3) ? 384 * 1024 : 0;
    }();
    if (bandBytes <= 0 || width <= 0)
        return 0;
    return std::max(8, bandBytes / (width * 4));
}

// The driver copies rows that span the frame at 0.56 ms a megapixel and
// narrower ones at 1.5 (zink on NVK, GTX 1070: 3840x1498 in 3.2 ms, the
// 2664x1498 of a video inside it in 5.9). A rectangle wider than that ratio
// of the frame is read as the band it lies in, and bands that touch or
// nearly touch are read as one. Only the read is widened: the damage the UI
// process draws stays what it was.
static Vector<IntRect, 1> widenedReadbackRectsHaiku(Vector<IntRect, 1>&& rects, const IntSize& frameSize)
{
    static constexpr double fullWidthCost = 0.56;
    static constexpr double partialWidthCost = 1.5;
    // A read costs a quarter of a millisecond before its first pixel.
    const int rowsWorthOneRead = static_cast<int>(0.25 / fullWidthCost * 1e6 / std::max(1, frameSize.width()));
    bool widened = false;
    for (auto& rect : rects) {
        if (rect.width() < frameSize.width() && rect.width() * partialWidthCost > frameSize.width() * fullWidthCost) {
            rect.setX(0);
            rect.setWidth(frameSize.width());
            widened = true;
        }
    }
    if (!widened || rects.size() < 2)
        return WTF::move(rects);
    Vector<IntRect, 1> bands;
    Vector<IntRect, 1> others;
    for (auto& rect : rects) {
        if (rect.width() == frameSize.width())
            bands.append(rect);
        else
            others.append(rect);
    }
    std::sort(bands.begin(), bands.end(), [](const IntRect& a, const IntRect& b) {
        return a.y() < b.y();
    });
    Vector<IntRect, 1> result;
    for (auto& band : bands) {
        if (!result.isEmpty() && band.y() <= result.last().maxY() + rowsWorthOneRead) {
            result.last().unite(band);
            continue;
        }
        result.append(band);
    }
    // What a band covers needs no read of its own; what lies within a few
    // rows of one joins it.
    for (auto& rect : others) {
        bool covered = false;
        for (auto& band : result) {
            if (rect.y() >= band.y() - rowsWorthOneRead && rect.maxY() <= band.maxY() + rowsWorthOneRead) {
                band.shiftYEdgeTo(std::min(band.y(), rect.y()));
                band.shiftMaxYEdgeTo(std::max(band.maxY(), rect.maxY()));
                covered = true;
                break;
            }
        }
        if (!covered)
            result.append(rect);
    }
    return result;
}

// Reading the frame straight into client memory made the driver copy it out of
// uncached staging memory and, as Skia asked for BGRA from an RGBA target,
// swizzle it on the CPU: 11.5 ms for a 4K frame. A pixel buffer object lets
// the GPU do the conversion into memory the CPU reads at full speed.
// SUMMIT_PBO_READBACK=0 returns to Skia's readPixels.
bool AcceleratedSurface::RenderTargetSHMImage::readBackThroughPixelBuffer()
{
    // A pixel buffer lets the GPU copy run ahead of the map, but zink on NVK
    // (the X399's GTX 1070) hands back zeros or an earlier frame from a reused
    // buffer, even after glFinish or a fence: every page was black. So by
    // default the changed rectangles are read straight into the shared bitmap
    // (below); SUMMIT_PBO_READBACK=1 selects the pixel buffer (fine on llvmpipe).
    static const bool pixelBuffer = [] {
        const char* value = getenv("SUMMIT_PBO_READBACK");
        return value && !strcmp(value, "1");
    }();
    // Say once why a frame could not take this path; it is the fast one.
    auto decline = [](const char* reason) {
        static std::once_flag once;
        std::call_once(once, [reason] { std::fprintf(stderr, "Summit readback: pixel buffer path not used: %s\n", reason); });
        return false;
    };
    // Skia's own readPixels reads a whole mirrored frame; only the pixel
    // buffer path reads part of one.
    if (!pixelBuffer && m_surface->useSkia())
        return false;
    // Skia compositing draws with its own context and must hand its work to
    // GL first; TextureMapper compositing leaves this target's framebuffer
    // bound in the current context.
    auto& display = PlatformDisplay::sharedDisplay();
    std::optional<GLContext::ScopedGLContextCurrent> skiaCurrent;
    GrDirectContext* grContext = nullptr;
    if (m_surface->useSkia()) {
        if (!m_skiaSurface)
            return decline("no Skia surface");
        auto* skiaContext = display.skiaGLContext();
        if (!skiaContext)
            return decline("no Skia GL context");
        skiaCurrent.emplace(*skiaContext);
        grContext = display.skiaGrContext();
        if (!grContext)
            return decline("no Skia GrDirectContext");
        grContext->flushAndSubmit(m_skiaSurface.get(), GrSyncCpu::kNo);
    }
    static const bool readsBGRA = [] {
        auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        return extensions && strstr(extensions, "GL_EXT_read_format_bgra");
    }();

    int width = m_bitmap->size().width();
    int height = m_bitmap->size().height();
    // The pixel buffer is packed; the shared bitmap may pad its rows.
    size_t rowBytes = size_t(width) * 4;
    size_t length = rowBytes * height;
    size_t destinationStride = m_bitmap->bytesPerRow();
    if (destinationStride < rowBytes || m_bitmap->mutableSpan().size() < destinationStride * height)
        return decline("the shared bitmap is smaller than the frame");

    // Only what changed since this buffer was last shown has to come back:
    // the rest of its memory still holds that frame. At 200% a whole frame is
    // 28 MB, and most frames change a caret, a video or an animation.
    IntRect whole { { }, m_bitmap->size() };
    Vector<IntRect, 1> rects;
#if ENABLE(DAMAGE_TRACKING)
    if (PresentStageStatsHaiku::enabled()) {
        static MonotonicTime lastDamageReport;
        auto now = MonotonicTime::now();
        if (now - lastDamageReport >= 2_s) {
            lastDamageReport = now;
            if (!m_readbackDamage)
                std::fprintf(stderr, "Summit readback damage: none recorded (whole frame)\n");
            else {
                auto bounds = m_readbackDamage->bounds();
                std::fprintf(stderr, "Summit readback damage: mode=%d rects=%zu bounds=%d,%d %dx%d:", static_cast<int>(m_readbackDamage->mode()),
                    m_readbackDamage->rects().size(), bounds.x(), bounds.y(), bounds.width(), bounds.height());
                size_t listed = 0;
                for (auto& rect : m_readbackDamage->rects()) {
                    if (listed++ == 12)
                        break;
                    std::fprintf(stderr, " %d,%d %dx%d", rect.x(), rect.y(), rect.width(), rect.height());
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    if (m_readbackDamage && m_readbackDamage->mode() != Damage::Mode::Full && !m_bitmapStale) {
        if (m_readbackDamage->isEmpty())
            return true;
        for (auto rect : m_readbackDamage->rects()) {
            rect.intersect(whole);
            if (!rect.isEmpty())
                rects.append(rect);
        }
        // Many small reads cost more than a few larger ones, but one bounding
        // box turned a scrollbar strip and an ad in the opposite corner into
        // the whole frame.
        rects = mergedReadbackRectsHaiku(WTF::move(rects));
        static const bool widen = [] {
            const char* value = getenv("SUMMIT_READBACK_BANDS");
            return !value || strcmp(value, "0");
        }();
        if (widen)
            rects = widenedReadbackRectsHaiku(WTF::move(rects), m_bitmap->size());
    } else
#endif
        rects.append(whole);

    bigtime_t readStart = PresentStageStatsHaiku::enabled() ? system_time() : 0;
    auto recordStats = [&] {
        if (!readStart)
            return;
        static NeverDestroyed<PresentStageStatsHaiku> read("pixels read (web)");
        double pixels = 0;
        for (auto& rect : rects)
            pixels += rect.width() * double(rect.height());
        read.get().add(system_time() - readStart, pixels / 1e6);
    };
    bool flipped = m_surface->shouldPaintMirrored();
    if (!pixelBuffer) {
        // Straight into the shared bitmap: each rectangle lands in place,
        // the rows the pack state skips are left as they were.
        if (flipped || !readsBGRA || destinationStride % 4)
            return decline("the frame is mirrored, BGRA reads are missing or rows are unaligned");
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        // SUMMIT_READBACK_SPLIT=1 (with SUMMIT_PRESENT_STATS=1) waits for the
        // GPU to finish compositing before it reads, which tells the time
        // the GPU spends drawing the frame from the time the read takes. The
        // megapixel column of that line is the number of reads.
        static const bool splitFinish = [] {
            const char* value = getenv("SUMMIT_READBACK_SPLIT");
            return value && !strcmp(value, "1");
        }();
        if (splitFinish && readStart) {
            glFinish();
            static NeverDestroyed<PresentStageStatsHaiku> finish("gpu finish, reads (web)");
            bigtime_t finished = system_time();
            finish.get().add(finished - readStart, rects.size());
            readStart = finished;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, destinationStride / 4);
        const int bandRows = readbackBandRowsHaiku(width);
        for (auto& rect : rects) {
            const int step = bandRows ? bandRows : rect.height();
            for (int y = rect.y(); y < rect.maxY(); y += step) {
                glPixelStorei(GL_PACK_SKIP_PIXELS, rect.x());
                glPixelStorei(GL_PACK_SKIP_ROWS, y);
                glReadPixels(rect.x(), y, rect.width(), std::min(step, rect.maxY() - y), GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_bitmap->mutableSpan().data());
            }
        }
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        recordStats();
        return true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    if (!m_pixelBuffer) {
        glGenBuffers(1, &m_pixelBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pixelBuffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, length, nullptr, GL_STREAM_READ);
    } else
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pixelBuffer);
    // Each rectangle lands where it belongs in a frame-sized buffer.
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, width);
    const int bandRows = readbackBandRowsHaiku(width);
    for (auto& rect : rects) {
        int glY = flipped ? height - rect.maxY() : rect.y();
        const int step = bandRows ? bandRows : rect.height();
        for (int y = glY; y < glY + rect.height(); y += step) {
            glPixelStorei(GL_PACK_SKIP_PIXELS, rect.x());
            glPixelStorei(GL_PACK_SKIP_ROWS, y);
            glReadPixels(rect.x(), y, rect.width(), std::min(step, glY + rect.height() - y), readsBGRA ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    bool copied = false;
    if (auto* pixels = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, length, GL_MAP_READ_BIT))) {
        auto* destination = m_bitmap->mutableSpan().data();
        for (auto& rect : rects) {
            size_t offset = size_t(rect.x()) * 4;
            size_t span = size_t(rect.width()) * 4;
            for (int row = rect.y(); row < rect.maxY(); ++row) {
                // Rows come bottom first when Skia draws the target mirrored.
                const uint8_t* source = pixels + size_t(flipped ? height - 1 - row : row) * rowBytes + offset;
                uint8_t* target = destination + size_t(row) * destinationStride + offset;
                if (readsBGRA) {
                    memcpy(target, source, span);
                    continue;
                }
                for (size_t x = 0; x < span; x += 4) {
                    target[x] = source[x + 2];
                    target[x + 1] = source[x + 1];
                    target[x + 2] = source[x];
                    target[x + 3] = source[x + 3];
                }
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        copied = true;
    } else
        decline("glMapBufferRange failed");
    recordStats();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    // Skia keeps its own record of the bound framebuffer and pack state.
    if (grContext)
        grContext->resetContext(kRenderTarget_GrGLBackendState | kPixelStore_GrGLBackendState | kMisc_GrGLBackendState);
    return copied;
}
#else
AcceleratedSurface::RenderTargetSHMImage::~RenderTargetSHMImage() = default;
#endif

#if PLATFORM(HAIKU)
bool AcceleratedSurface::RenderTargetSHMImage::presentDirectlyHaiku(DirectPresenterHaiku* presenter, const std::optional<Damage>& damage)
{
    m_presentedDirectly = false;
    if (!presenter || !presenter->wanted() || m_surface->useSkia() || !m_surface->usesGL() || !m_fbo
        || m_surface->shouldPaintMirrored())
        return false;
    IntRect whole { { }, m_size };
    Vector<IntRect, 1> rects;
    if (damage && damage->mode() != Damage::Mode::Full) {
        for (auto rect : damage->rects()) {
            rect.intersect(whole);
            if (!rect.isEmpty())
                rects.append(rect);
        }
    } else
        rects.append(whole);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    if (!presenter->present(m_fbo, m_size, rects))
        return false;
    m_presentedDirectly = true;
    m_bitmapStale = true;
    return true;
}

void AcceleratedSurface::RenderTargetSHMImage::sendFrame(Vector<IntRect, 1>&& damageRects)
{
    if (!m_presentedDirectly) {
        RenderTargetShareableBuffer::sendFrame(WTF::move(damageRects));
        return;
    }
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit swapchain: surface=%llu send=%llu direct\n", static_cast<unsigned long long>(m_surface->surfaceID()), static_cast<unsigned long long>(m_id));
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DirectFrame(m_id), m_surface->surfaceID());
}
#endif

void AcceleratedSurface::RenderTargetSHMImage::didRenderFrame()
{
#if PLATFORM(HAIKU)
    // The frame is on the screen already.
    if (m_presentedDirectly)
        return;
    // Whatever path reads the frame below reads all of it when the bitmap
    // missed frames presented directly.
    auto fresh = makeScopeExit([this] { m_bitmapStale = false; });
    if (PresentStageStatsHaiku::enabled()) {
        static std::once_flag once;
        std::call_once(once, [this] { std::fprintf(stderr, "Summit readback: %s compositing\n", m_surface->useSkia() ? (m_surface->usesGL() ? "Skia GL" : "Skia CPU") : "TextureMapper GL"); });
    }
#endif
    if (m_surface->useSkia()) {
        if (!m_skiaSurface)
            return;
#if PLATFORM(HAIKU)
        if (m_surface->usesGL() && readBackThroughPixelBuffer())
            return;
#endif

        SkImageInfo info = SkImageInfo::Make(m_bitmap->size().width(), m_bitmap->size().height(), SkColorType::kBGRA_8888_SkColorType, SkAlphaType::kPremul_SkAlphaType);

        if (m_surface->usesGL()) {
            GLContext::ScopedGLContextCurrent scopedCurrent(*PlatformDisplay::sharedDisplay().skiaGLContext());
            m_skiaSurface->readPixels(info, m_bitmap->mutableSpan().data(), m_bitmap->bytesPerRow(), 0, 0);
        } else
            m_skiaSurface->readPixels(info, m_bitmap->mutableSpan().data(), m_bitmap->bytesPerRow(), 0, 0);
        return;
    }

#if PLATFORM(HAIKU)
    if (readBackThroughPixelBuffer())
        return;
#endif
    glReadPixels(0, 0, m_bitmap->size().width(), m_bitmap->size().height(), GL_BGRA, GL_UNSIGNED_BYTE, m_bitmap->mutableSpan().data());
}

#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM)
std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::RenderTargetTexture::create(AcceleratedSurface& surface, const IntSize& size)
{
    auto texture = BitmapTexture::create(size);

    auto& display = PlatformDisplay::sharedDisplay();
    auto image = display.createEGLImage(eglGetCurrentContext(), EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uint64_t)texture->id(), { });
    if (!image) {
        WTFLogAlways("Failed to create EGL image for texture");
        return nullptr;
    }

    int fourcc, planeCount;
    uint64_t modifier;
    if (!eglExportDMABUFImageQueryMESA(display.eglDisplay(), image, &fourcc, &planeCount, &modifier)) {
        WTFLogAlways("eglExportDMABUFImageQueryMESA failed");
        display.destroyEGLImage(image);
        return nullptr;
    }

    Vector<int> fdsOut(planeCount);
    Vector<int> stridesOut(planeCount);
    Vector<int> offsetsOut(planeCount);
    if (!eglExportDMABUFImageMESA(display.eglDisplay(), image, fdsOut.mutableSpan().data(), stridesOut.mutableSpan().data(), offsetsOut.mutableSpan().data())) {
        WTFLogAlways("eglExportDMABUFImageMESA failed");
        display.destroyEGLImage(image);
        return nullptr;
    }

    display.destroyEGLImage(image);

    Vector<UnixFileDescriptor> fds = fdsOut.map([](int fd) {
        return UnixFileDescriptor(fd, UnixFileDescriptor::Adopt);
    });
    Vector<uint32_t> strides = stridesOut.map([](int stride) {
        return static_cast<uint32_t>(stride);
    });
    Vector<uint32_t> offsets = offsetsOut.map([](int offset) {
        return static_cast<uint32_t>(offset);
    });

    return makeUnique<RenderTargetTexture>(surface, size, WTF::move(texture), fourcc, WTF::move(fds), WTF::move(offsets), WTF::move(strides), modifier);
}

AcceleratedSurface::RenderTargetTexture::RenderTargetTexture(AcceleratedSurface& surface, const IntSize& size, Ref<BitmapTexture>&& texture, uint32_t format, Vector<UnixFileDescriptor>&& fds, Vector<uint32_t>&& offsets, Vector<uint32_t>&& strides, uint64_t modifier)
    : RenderTargetShareableBuffer(surface, size)
    , m_texture(WTF::move(texture))
{
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture->id(), 0);
    if (m_surface->useSkia())
        createSkiaSurfaceForFramebuffer(m_fbo);

    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateDMABufBuffer(m_id, { size, format, WTF::move(fds), WTF::move(offsets), WTF::move(strides), modifier }, RendererBufferFormat::Usage::Rendering), m_surface->surfaceID());
}

AcceleratedSurface::RenderTargetTexture::~RenderTargetTexture() = default;
#endif

#endif // PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)

#if USE(WPE_RENDERER)
std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::RenderTargetWPEBackend::create(AcceleratedSurface& surface, const IntSize& initialSize, UnixFileDescriptor&& hostFD)
{
    return makeUnique<RenderTargetWPEBackend>(surface, initialSize, WTF::move(hostFD));
}

AcceleratedSurface::RenderTargetWPEBackend::RenderTargetWPEBackend(AcceleratedSurface& surface, const IntSize& initialSize, UnixFileDescriptor&& hostFD)
    : RenderTarget(surface, initialSize)
{
    ASSERT(hostFD, "RenderTargetWPEBackend created with invalid host FD");
    m_backend = wpe_renderer_backend_egl_target_create(hostFD.release());
    static struct wpe_renderer_backend_egl_target_client s_client = {
        // frame_complete
        [](void* data) {
            auto& surface = *reinterpret_cast<AcceleratedSurface*>(data);
            surface.frameDone();
        },
        // padding
        nullptr,
        nullptr,
        nullptr,
        nullptr
    };
    wpe_renderer_backend_egl_target_set_client(m_backend, &s_client, const_cast<AcceleratedSurface*>(&surface));
    wpe_renderer_backend_egl_target_initialize(m_backend, downcast<PlatformDisplayLibWPE>(PlatformDisplay::sharedDisplay()).backend(),
        std::max(1, m_size.width()), std::max(1, m_size.height()));
}

AcceleratedSurface::RenderTargetWPEBackend::~RenderTargetWPEBackend()
{
#if WPE_CHECK_VERSION(1, 9, 1)
    // libwpe 1.9.1 introduced an additional ::deinitialize function, which
    // may be called some time before destruction. As there is no better place
    // to invoke it at the moment, do it right before destroying the object.
    wpe_renderer_backend_egl_target_deinitialize(m_backend);
#endif

    wpe_renderer_backend_egl_target_destroy(m_backend);
}

uint64_t AcceleratedSurface::RenderTargetWPEBackend::window() const
{
    // EGLNativeWindowType changes depending on the EGL implementation: reinterpret_cast works
    // for pointers (only if they are 64-bit wide and not for other cases), and static_cast for
    // numeric types (and when needed they get extended to 64-bit) but not for pointers. Using
    // a plain C cast expression in this one instance works in all cases.
    static_assert(sizeof(EGLNativeWindowType) <= sizeof(uint64_t), "EGLNativeWindowType must not be longer than 64 bits.");
    return (uint64_t)wpe_renderer_backend_egl_target_get_native_window(m_backend);
}

void AcceleratedSurface::RenderTargetWPEBackend::resize(const IntSize& size)
{
    m_size = size;
    wpe_renderer_backend_egl_target_resize(m_backend, std::max(1, m_size.width()), std::max(1, m_size.height()));
    m_skiaSurface = nullptr;
}

void AcceleratedSurface::RenderTargetWPEBackend::willRenderFrame()
{
    if (m_surface->useSkia() && !m_skiaSurface) {
        GLint fbo;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        createSkiaSurfaceForFramebuffer(fbo);
    }
    wpe_renderer_backend_egl_target_frame_will_render(m_backend);
}

void AcceleratedSurface::RenderTargetWPEBackend::didRenderFrame()
{
    wpe_renderer_backend_egl_target_frame_rendered(m_backend);
}
#endif

AcceleratedSurface::SwapChain::SwapChain(AcceleratedSurface& surface)
    : m_surface(surface)
{
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
    if (!m_surface->hardwareAccelerationEnabled()) {
        m_type = Type::SharedMemory;
        return;
    }
#endif

    auto& display = PlatformDisplay::sharedDisplay();
    switch (display.type()) {
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM)
    case PlatformDisplay::Type::Surfaceless:
        if (display.eglExtensions().MESA_image_dma_buf_export && WebProcess::singleton().rendererBufferTransportMode().contains(RendererBufferTransportMode::Hardware))
            m_type = Type::Texture;
        else
            m_type = Type::SharedMemory;
        break;
#if USE(GBM)
    case PlatformDisplay::Type::GBM:
        if (display.eglExtensions().EXT_image_dma_buf_import) {
            m_type = Type::EGLImage;
            setupBufferFormat();
        } else
            m_type = Type::SharedMemory;
        break;
#endif
#if OS(ANDROID)
    case PlatformDisplay::Type::Android:
    case PlatformDisplay::Type::Default:
        m_type = Type::EGLImage;
        setupBufferFormat();
        break;
#endif
#else
    case PlatformDisplay::Type::Surfaceless:
#if USE(GBM)
    case PlatformDisplay::Type::GBM:
#endif
        break;
#endif // PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
#if USE(WPE_RENDERER)
    case PlatformDisplay::Type::WPE:
        m_type = Type::WPEBackend;
        Ref webPage { m_surface->m_webPage };
        m_initialSize = webPage->size();
        m_initialSize.scale(webPage->deviceScaleFactor());
        m_hostFD = webPage->hostFileDescriptor();
        break;
#endif
#if PLATFORM(GTK)
    case PlatformDisplay::Type::Default:
        break;
#endif // PLATFORM(GTK)
#if PLATFORM(HAIKU)
    case PlatformDisplay::Type::Haiku:
        // The shared memory target is the portable one, and it is what the port
        // already hands to the UI process every frame: a ShareableBitmap whose
        // pixels the app copies into a BBitmap.
        m_type = Type::SharedMemory;
        break;
#endif

    }
}

#if (PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)) && (USE(GBM) || OS(ANDROID))
void AcceleratedSurface::SwapChain::setupBufferFormat()
{
    if (m_type != Type::EGLImage)
        return;

    auto isOpaqueFormat = [](FourCC fourcc) -> bool {
        return fourcc != DRM_FORMAT_ARGB8888
            && fourcc != DRM_FORMAT_RGBA8888
            && fourcc != DRM_FORMAT_ABGR8888
            && fourcc != DRM_FORMAT_BGRA8888
            && fourcc != DRM_FORMAT_ARGB2101010
            && fourcc != DRM_FORMAT_ABGR2101010
            && fourcc != DRM_FORMAT_ARGB16161616F
            && fourcc != DRM_FORMAT_ABGR16161616F;
    };

    Ref webPage { m_surface->m_webPage };
    bool isOpaque = m_surface->isOpaque();

    // The preferred formats vector is sorted by usage, but all formats for the same usage has the same priority.
    Locker locker { m_bufferFormatLock };
    BufferFormat newBufferFormat;
    const auto& supportedFormats = PlatformDisplay::sharedDisplay().bufferFormats();
    for (const auto& bufferFormat : webPage->preferredBufferFormats()) {

        auto matchesOpacity = false;
        for (const auto& format : supportedFormats) {
            auto index = bufferFormat.formats.findIf([&](const auto& item) {
                return format.fourcc == item.fourcc;
            });
            if (index != notFound) {
                const auto& preferredFormat = bufferFormat.formats[index];

                matchesOpacity = isOpaqueFormat(preferredFormat.fourcc) == isOpaque;
                if (!matchesOpacity && newBufferFormat.fourcc)
                    continue;

                newBufferFormat.usage = bufferFormat.usage;
                newBufferFormat.fourcc = preferredFormat.fourcc;
#if USE(GBM)
                newBufferFormat.drmDevice = bufferFormat.drmDevice;
                if (preferredFormat.modifiers[0] == DRM_FORMAT_MOD_INVALID)
                    newBufferFormat.modifiers = preferredFormat.modifiers;
                else {
                    newBufferFormat.modifiers = WTF::compactMap(preferredFormat.modifiers, [&format](uint64_t modifier) -> std::optional<uint64_t> {
                        if (format.modifiers.contains(modifier))
                            return modifier;
                        return std::nullopt;
                    });
                }
#endif // USE(GBM)

                if (matchesOpacity)
                    break;
            }
        }

        if (newBufferFormat.fourcc && matchesOpacity)
            break;
    }

    if (!newBufferFormat.fourcc || newBufferFormat == m_bufferFormat)
        return;

#if USE(GBM)
    if (!newBufferFormat.drmDevice.isNull()) {
        if (newBufferFormat.drmDevice == m_bufferFormat.drmDevice && newBufferFormat.usage == m_bufferFormat.usage)
            newBufferFormat.gbmDevice = m_bufferFormat.gbmDevice;
        else {
            auto nodeType = newBufferFormat.usage == RendererBufferFormat::Usage::Scanout ? DRMDeviceManager::NodeType::Primary : DRMDeviceManager::NodeType::Render;
            newBufferFormat.gbmDevice = DRMDeviceManager::singleton().gbmDevice(newBufferFormat.drmDevice, nodeType);
        }
    }
#endif // USE(GBM)

    m_bufferFormat = WTF::move(newBufferFormat);
    m_bufferFormatChanged = true;
}
#endif // USE(GBM) || OS(ANDROID)

bool AcceleratedSurface::SwapChain::resize(const IntSize& size)
{
    if (m_size == size)
        return false;

    m_size = size;
#if USE(WPE_RENDERER)
    if (m_type == Type::WPEBackend) {
        static_cast<RenderTargetWPEBackend*>(m_lockedTargets[0].get())->resize(m_size);
        return true;
    }
#endif
    reset();
    return true;
}

bool AcceleratedSurface::SwapChain::handleBufferFormatChangeIfNeeded()
{
#if (PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)) && (USE(GBM) || OS(ANDROID))
    if (m_type == Type::EGLImage) {
        Locker locker { m_bufferFormatLock };
        if (m_bufferFormatChanged) {
            reset();
            m_bufferFormatChanged = false;
            return true;
        }
    }
#endif
    return false;
}

std::unique_ptr<AcceleratedSurface::RenderTarget> AcceleratedSurface::SwapChain::createTarget() const
{
    switch (m_type) {
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
#if USE(GBM) || OS(ANDROID)
    case Type::EGLImage: {
        Locker locker { m_bufferFormatLock };
        return RenderTargetEGLImage::create(m_surface.get(), m_size, m_bufferFormat);
    }
#endif
    case Type::Texture:
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM)
        return RenderTargetTexture::create(m_surface.get(), m_size);
#else
        break;
#endif
    case Type::SharedMemory:
        return RenderTargetSHMImage::create(m_surface.get(), m_size);
#endif
#if USE(WPE_RENDERER)
    case Type::WPEBackend:
        ASSERT_NOT_REACHED();
        break;
#endif
    case Type::Invalid:
        break;
    }
    return nullptr;
}

AcceleratedSurface::RenderTarget* AcceleratedSurface::SwapChain::nextTarget()
{
#if USE(WPE_RENDERER)
    if (m_type == Type::WPEBackend)
        return m_lockedTargets[0].get();
#endif

    if (m_freeTargets.isEmpty()) {
        ASSERT(m_lockedTargets.size() < s_maximumBuffers);

        unsigned targetsToCreate = 1;
        if (!m_initialTargetsCreated) {
            targetsToCreate = s_initialBuffers;
            m_initialTargetsCreated = true;
        }

#if ENABLE(WPE_PLATFORM)
        WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidChangeBufferConfiguration(m_lockedTargets.size() + targetsToCreate), m_surface->surfaceID());
#endif

        for (unsigned i = 0; i < targetsToCreate; ++i) {
            if (auto target = createTarget())
                m_freeTargets.append(WTF::move(target));
        }
#if PLATFORM(HAIKU)
        if (swapChainTraceEnabled())
            std::fprintf(stderr, "Summit swapchain: surface=%llu create=%u locked=%zu free=%zu size=%dx%d\n",
                static_cast<unsigned long long>(m_surface->surfaceID()), targetsToCreate, m_lockedTargets.size(), m_freeTargets.size(), m_size.width(), m_size.height());
#endif

#if ENABLE(WPE_PLATFORM)
        if (m_freeTargets.size() != targetsToCreate)
            WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidChangeBufferConfiguration(m_lockedTargets.size() + m_freeTargets.size()), m_surface->surfaceID());
#endif
    }

    if (m_freeTargets.isEmpty())
        return nullptr;

    auto target = m_freeTargets.takeLast();
    m_lockedTargets.insert(0, WTF::move(target));
    return m_lockedTargets[0].get();
}

void AcceleratedSurface::SwapChain::releaseTarget(uint64_t targetID, UnixFileDescriptor&& releaseFence)
{
#if USE(WPE_RENDERER)
    ASSERT(m_type != Type::WPEBackend);
#endif

    auto index = m_lockedTargets.reverseFindIf([targetID](const auto& item) {
        return item->id() == targetID;
    });
    if (index != notFound) {
        m_lockedTargets[index]->setReleaseFenceFD(WTF::move(releaseFence));
        m_freeTargets.insert(0, WTF::move(m_lockedTargets[index]));
        m_lockedTargets.removeAt(index);
    }
#if PLATFORM(HAIKU)
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit swapchain: surface=%llu release=%llu %s locked=%zu free=%zu\n",
            static_cast<unsigned long long>(m_surface->surfaceID()), static_cast<unsigned long long>(targetID),
            index != notFound ? "found" : "unknown", m_lockedTargets.size(), m_freeTargets.size());
#endif
}

void AcceleratedSurface::SwapChain::reset()
{
#if ENABLE(WPE_PLATFORM)
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidChangeBufferConfiguration(0), m_surface->surfaceID());
#endif

    m_lockedTargets.clear();
    m_freeTargets.clear();
    m_initialTargetsCreated = false;
}

void AcceleratedSurface::SwapChain::releaseUnusedBuffers()
{
#if USE(WPE_RENDERER)
    ASSERT(m_type != Type::WPEBackend);
#endif
#if PLATFORM(HAIKU)
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit swapchain: surface=%llu trim locked=%zu free=%zu\n",
            static_cast<unsigned long long>(m_surface->surfaceID()), m_lockedTargets.size(), m_freeTargets.size());
#endif

#if ENABLE(WPE_PLATFORM)
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidChangeBufferConfiguration(m_lockedTargets.size()), m_surface->surfaceID());
#endif

    m_freeTargets.clear();
}

#if USE(WPE_RENDERER)
uint64_t AcceleratedSurface::SwapChain::window()
{
    ASSERT(m_type == Type::WPEBackend);
    if (m_lockedTargets.isEmpty())
        m_lockedTargets.append(RenderTargetWPEBackend::create(m_surface.get(), m_initialSize, WTF::move(m_hostFD)));
    return static_cast<RenderTargetWPEBackend*>(m_lockedTargets[0].get())->window();
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void AcceleratedSurface::SwapChainDamageTracker::recordFrameDamage(Damage&& damage, AccumulateIntoSwapChain accumulate)
{
    m_frameDamage = WTF::move(damage);

    // Add this frame's damage to every target, so each one repaints what changed since it was last
    // rendered. This must happen on record rather than on read, since a frame that never reads its
    // damage still has to contribute to the other targets.
    if (accumulate == AccumulateIntoSwapChain::Yes) {
        m_swapChain.forEachTarget([&](RenderTarget& target) {
            target.addDamage(*m_frameDamage);
        });
    }
}

Vector<IntRect, 1> AcceleratedSurface::SwapChainDamageTracker::takeFrameDamageRects()
{
    if (!m_frameDamage)
        return { };

    const auto frameDamage = *std::exchange(m_frameDamage, std::nullopt);
    if (frameDamage.mode() != Damage::Mode::Rectangles || m_swapChain.size().isEmpty())
        return frameDamage.rects();

    // The frame damage is fine-grained, because the compositing restriction needs it that way, but the
    // platform only wants a bounded number of rects. Merge it down to the threshold here, which is the
    // only thing the threshold bounds.
    Damage platformDamage(m_swapChain.size(), Damage::Mode::Rectangles, m_rectangleThreshold);
    platformDamage.add(frameDamage);
    return platformDamage.rects();
}

void AcceleratedSurface::SwapChainDamageTracker::didPresent(RenderTarget& target, TargetContents targetContents)
{
    if (targetContents == TargetContents::Invalid) {
        // Nothing was drawn, so drop the record and let the target's next use repaint it fully.
        target.setDamage(std::nullopt);
        return;
    }

    // The target is now current, so start a fresh, empty record. NoMaxRectangles keeps the grid as
    // fine as the mode allows, since on a wide, short surface the threshold would collapse it into a
    // few full-height cells.
    target.setDamage(Damage(m_swapChain.size(), needsFineGrainedDamage() ? Damage::Mode::Rectangles : Damage::Mode::BoundingBox, Damage::NoMaxRectangles));
}
#endif

bool AcceleratedSurface::shouldPaintMirrored() const
{
    switch (m_swapChain.type()) {
    case SwapChain::Type::Invalid:
        return false;
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
    case SwapChain::Type::Texture:
#if USE(GBM) || OS(ANDROID)
    case SwapChain::Type::EGLImage:
#endif
#if PLATFORM(GTK) && !USE(GTK4)
        return true;
#else
        return false;
#endif
    case SwapChain::Type::SharedMemory:
        return false;
#endif
#if USE(WPE_RENDERER)
    case SwapChain::Type::WPEBackend:
        return true;
#endif
    }

    RELEASE_ASSERT_NOT_REACHED();
}

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void AcceleratedSurface::preferredBufferFormatsDidChange()
{
    m_swapChain.setupBufferFormat();
}
#endif

void AcceleratedSurface::visibilityDidChange(bool isVisible)
{
#if PLATFORM(HAIKU)
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit swapchain: surface=%llu visible=%d was=%d\n", static_cast<unsigned long long>(m_id), isVisible, m_isVisible);
#endif
    if (m_isVisible == isVisible)
        return;

    m_isVisible = isVisible;

    // m_releaseUnusedBuffersTimer is owned by, and fires on, the compositing run loop; it must be
    // started/stopped there. visibilityDidChange() is called on the main thread (from
    // ThreadedCompositor::suspend()/resume()), so hop to the compositing run loop.
    RefPtr compositingRunLoop = m_compositingRunLoop;
    if (!compositingRunLoop)
        return;

    compositingRunLoop->dispatch([protectedThis = Ref { *this }, isVisible] {
        auto& timer = protectedThis->m_releaseUnusedBuffersTimer;
        if (!timer)
            return;
        if (isVisible)
            timer->stop();
        else {
            static const Seconds releaseUnusedBuffersDelay = 10_s;
            timer->startOneShot(releaseUnusedBuffersDelay);
        }
    });
}

void AcceleratedSurface::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    Ref webPage = m_webPage;
    const auto& color = webPage->backgroundColor();

    bool wasOpaque = this->isOpaque();
    {
        Locker locker { m_backgroundColorLock };
        m_backgroundColor = color;
    }
    bool isOpaque = this->isOpaque();
    if (isOpaque == wasOpaque)
        return;

#if (PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)) && (USE(GBM) || OS(ANDROID))
    m_swapChain.setupBufferFormat();
#endif
}

void AcceleratedSurface::releaseUnusedBuffersTimerFired()
{
    m_swapChain.releaseUnusedBuffers();
}

void AcceleratedSurface::didCreateCompositingRunLoop(RunLoop& runLoop)
{
#if USE(WPE_RENDERER)
    if (m_swapChain.type() == SwapChain::Type::WPEBackend)
        return;
#endif

    m_compositingRunLoop = &runLoop;
    m_releaseUnusedBuffersTimer = makeUnique<RunLoop::Timer>(runLoop, "AcceleratedSurface::ReleaseUnusedBuffersTimer"_s, this, &AcceleratedSurface::releaseUnusedBuffersTimerFired);
#if USE(GLIB_EVENT_LOOP)
    m_releaseUnusedBuffersTimer->setPriority(RunLoopSourcePriority::ReleaseUnusedResourcesTimer);
#endif
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
    WebProcess::singleton().parentProcessConnection()->addMessageReceiver(runLoop, *this, Messages::AcceleratedSurface::messageReceiverName(), m_id);
#endif
}

void AcceleratedSurface::willDestroyCompositingRunLoop()
{
    m_frameCompleteHandler = nullptr;
#if PLATFORM(HAIKU)
    m_renderTargetReleasedHandler = nullptr;
    m_redisplayHandlerHaiku = nullptr;
#endif

#if USE(WPE_RENDERER)
    if (m_swapChain.type() == SwapChain::Type::WPEBackend)
        return;
#endif

    // m_releaseUnusedBuffersTimer is owned by the compositing run loop and its destructor stops it, so
    // destroy it on that run loop's thread rather than here on the main thread.
    if (RefPtr compositingRunLoop = std::exchange(m_compositingRunLoop, nullptr)) {
        compositingRunLoop->dispatch([protectedThis = Ref { *this }] {
            protectedThis->m_releaseUnusedBuffersTimer = nullptr;
        });
    }
#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
    WebProcess::singleton().parentProcessConnection()->removeMessageReceiver(Messages::AcceleratedSurface::messageReceiverName(), m_id);
#endif
}

void AcceleratedSurface::willDestroyGLContext()
{
    m_pendingFrameNotifyTargets.clear();
    m_target = nullptr;
    m_swapChain.reset();
}

uint64_t AcceleratedSurface::window()
{
#if USE(WPE_RENDERER)
    if (m_swapChain.type() == SwapChain::Type::WPEBackend)
        return m_swapChain.window();
#endif
    return 0;
}

bool AcceleratedSurface::isOpaque() const
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_backgroundColorLock };
    return !m_backgroundColor || m_backgroundColor->isOpaque();
}

SkCanvas* AcceleratedSurface::canvas()
{
    if (!m_useSkia)
        return nullptr;

    if (auto* surface = m_target ? m_target->skiaSurface() : nullptr)
        return surface->getCanvas();

    return nullptr;
}

void AcceleratedSurface::willRenderFrame(const IntSize& size)
{
    bool sizeDidChange = m_swapChain.resize(size);
    bool bufferFormatChanged = m_swapChain.handleBufferFormatChangeIfNeeded();
    if (sizeDidChange || bufferFormatChanged) {
        m_pendingFrameNotifyTargets.clear();
#if ENABLE(DAMAGE_TRACKING)
        m_damageTracker.reset();
#endif
    }

    m_target = m_swapChain.nextTarget();
    if (m_target)
        m_target->willRenderFrame();

    if (sizeDidChange && !m_useSkia)
        glViewport(0, 0, size.width(), size.height());
}

std::optional<Color> AcceleratedSurface::backgroundColor()
{
    Locker locker { m_backgroundColorLock };
    return m_backgroundColor;
}

std::optional<SkColor> AcceleratedSurface::skiaClearColor(const OptionSet<WebCore::CompositionReason>& reasons)
{
    const auto backgroundColor = this->backgroundColor();
    if (backgroundColor && !backgroundColor->isOpaque())
        return SK_ColorTRANSPARENT;

    if (reasons.contains(CompositionReason::AsyncScrolling))
        return backgroundColor ? SkColor(*backgroundColor) : SK_ColorWHITE;

    return std::nullopt;
}

void AcceleratedSurface::clear(const OptionSet<WebCore::CompositionReason>& reasons)
{
    if (m_useSkia) {
        if (auto clearColor = skiaClearColor(reasons)) {
            if (auto* canvas = this->canvas())
                canvas->clear(*clearColor);
        }
        return;
    }

    const auto backgroundColor = this->backgroundColor();
    if (backgroundColor && !backgroundColor->isOpaque()) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    if (reasons.contains(CompositionReason::AsyncScrolling)) {
        if (backgroundColor) {
            auto [r, g, b, a] = backgroundColor->toResolvedColorComponentsInColorSpace(WebCore::ColorSpaceName::SRGB);
            glClearColor(r, g, b, a);
        } else
            glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void AcceleratedSurface::didRenderFrame(TargetContents targetContents)
{
#if PLATFORM(GTK) || PLATFORM(WPE)
    TraceScope traceScope(WaitForCompositionCompletionStart, WaitForCompositionCompletionEnd);
#endif

    if (!m_target)
        return;

#if PLATFORM(HAIKU)
    bigtime_t renderStart = PresentStageStatsHaiku::enabled() ? system_time() : 0;
    // Taken before didPresent() starts the target's next record. Only a frame
    // that recorded damage keeps the targets' records current: with damage
    // tracking off each record stays the empty one didPresent() started, which
    // read as "nothing changed" and left the page black. Read it all then.
    std::optional<Damage> targetDamage;
#if ENABLE(DAMAGE_TRACKING)
    if (m_damageTracker.frameDamage())
        targetDamage = m_damageTracker.damageSinceTargetWasLastCurrent(*m_target);
#endif
    // Straight into the screen, if the UI process gave the page a place there:
    // then there is nothing to wait for and nothing to read back. app_server's
    // own copy of the screen (screenshots, the VNC server, windows moved away)
    // is not written that way, so a changed page still goes through app_server
    // a few times a second, the frame read back as before: the same pixels
    // the screen has, drawn once more. Never the first frame that changes
    // after a pause, though: that is the frame someone waits for (a page's
    // first paint, the answer to a click), and reading it back (84 ms for a
    // full 3824x2104 frame) delayed it. app_server's copy catches up within a
    // quarter of a second.
    bool changed = !targetDamage || !targetDamage->isEmpty();
    auto now = MonotonicTime::now();
    bool throughAppServer = m_directPresenter && m_directPagesPendingHaiku
        && now - m_directPagesPendingSinceHaiku >= 250_ms && now - m_lastFrameThroughAppServerHaiku >= 250_ms;
    // Without a presenter the target notes that this frame is read back.
    bool presentedDirectly = m_target->presentDirectlyHaiku(throughAppServer ? nullptr : m_directPresenter.get(), targetDamage);
    if (presentedDirectly) {
        if (changed && !m_directPagesPendingHaiku) {
            m_directPagesPendingHaiku = true;
            m_directPagesPendingSinceHaiku = now;
            // A page that stops changing still gets its last frame there.
            if (m_compositingRunLoop) {
                m_compositingRunLoop->dispatchAfter(300_ms, [weakThis = ThreadSafeWeakPtr { *this }] {
                    if (RefPtr protectedThis = weakThis.get(); protectedThis && protectedThis->m_directPagesPendingHaiku && protectedThis->m_redisplayHandlerHaiku)
                        protectedThis->m_redisplayHandlerHaiku();
                });
            }
        }
    } else if (m_directPresenter && (throughAppServer || !m_directPresenter->wanted())) {
        m_lastFrameThroughAppServerHaiku = now;
        m_directPagesPendingHaiku = false;
    }
    if (usesGL() && !presentedDirectly)
        m_target->sync(m_useExplicitSync);
    bigtime_t readbackStart = renderStart ? system_time() : 0;
#else
    if (usesGL())
        m_target->sync(m_useExplicitSync);
#endif

    Vector<IntRect, 1> damageRects;
#if ENABLE(DAMAGE_TRACKING)
#if PLATFORM(HAIKU)
    m_target->setReadbackDamage(WTF::move(targetDamage));
#endif
    m_damageTracker.didPresent(*m_target, targetContents);
    damageRects = m_damageTracker.takeFrameDamageRects();
#if PLATFORM(HAIKU)
    // No rectangles would read as "the whole frame" anyway; say so outright.
    if (damageRects.isEmpty())
        damageRects.append(IntRect { { }, m_swapChain.size() });
    // app_server's own copy of the screen gets only frames that go through
    // it, and each names only what changed since the frame before it. What
    // frames presented directly changed in between is drawn there too, or
    // that copy keeps the old pixels for good, and app_server copies them
    // back onto the screen whenever it redraws the window (another window
    // moving off it, a menu closing), as well as into screenshots.
    if (m_directPresenter) {
        if (presentedDirectly) {
            for (auto& rect : damageRects)
                m_directDamageHaiku.unite(rect);
        } else if (!m_directDamageHaiku.isEmpty()) {
            for (auto& rect : damageRects)
                m_directDamageHaiku.unite(rect);
            auto rects = m_directDamageHaiku.rects();
            if (rects.size() > 64)
                damageRects = { m_directDamageHaiku.bounds() };
            else
                damageRects = WTF::move(rects);
            m_directDamageHaiku = { };
        }
    }
#endif
#else
    UNUSED_PARAM(targetContents);
#endif

    m_target->didRenderFrame();
#if PLATFORM(HAIKU)
    if (renderStart) {
        static NeverDestroyed<PresentStageStatsHaiku> gpuWait("gpu-wait (web)");
        static NeverDestroyed<PresentStageStatsHaiku> readback("readback (web)");
        double megapixels = m_swapChain.size().width() * double(m_swapChain.size().height()) / 1e6;
        bigtime_t now = system_time();
        gpuWait.get().add(readbackStart - renderStart, megapixels);
        readback.get().add(now - readbackStart, megapixels);
    }
#endif
    m_pendingFrameNotifyTargets.insert(0, { m_target, WTF::move(damageRects) });
    m_target = nullptr;
}

void AcceleratedSurface::sendFrame()
{
    if (m_pendingFrameNotifyTargets.isEmpty())
        return;

    auto [target, damageRects] = m_pendingFrameNotifyTargets.takeLast();
    target->sendFrame(WTF::move(damageRects));
}

#if ENABLE(DAMAGE_TRACKING)
const std::optional<Damage>& AcceleratedSurface::renderTargetDamage()
{
    static std::optional<Damage> nulloptDamage;
    return m_target ? m_damageTracker.damageSinceTargetWasLastCurrent(*m_target) : nulloptDamage;
}
#endif

#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM) || PLATFORM(HAIKU)
void AcceleratedSurface::releaseBuffer(uint64_t targetID, UnixFileDescriptor&& releaseFence)
{
#if USE(WPE_RENDERER)
    ASSERT(m_swapChain.type() != SwapChain::Type::WPEBackend);
#endif
    m_swapChain.releaseTarget(targetID, WTF::move(releaseFence));
#if PLATFORM(HAIKU)
    if (m_renderTargetReleasedHandler)
        m_renderTargetReleasedHandler();
#endif
}
#endif

void AcceleratedSurface::frameDone()
{
    if (m_frameCompleteHandler)
        m_frameCompleteHandler();
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
