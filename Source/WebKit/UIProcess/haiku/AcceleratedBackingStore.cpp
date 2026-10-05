/*
 * Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AcceleratedBackingStore.h"

#if USE(COORDINATED_GRAPHICS)

#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#include "DirectPresentTargetHaiku.h"
#include "StartupTraceHaiku.h"
#include "PageClient.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include <Bitmap.h>
#include <OS.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/unix/UnixFileDescriptor.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedBackingStore);

// Stores by an identifier of this process's own, so a frame released on a
// window thread can find its way back. Surface identifiers only count within
// one web process: keyed by those, every tab's first surface was "1", and a
// buffer given back went to another tab's store (and was lost), until the web
// process had made hundreds. Main thread only.
static HashMap<uint64_t, AcceleratedBackingStore*>& storesByIdentifier()
{
    static NeverDestroyed<HashMap<uint64_t, AcceleratedBackingStore*>> stores;
    return stores;
}

static uint64_t nextStoreIdentifier()
{
    static uint64_t identifier;
    return ++identifier;
}

// SUMMIT_ZERO_COPY_PRESENT=0 copies every frame into a private BBitmap again.
static bool zeroCopyPresentEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_ZERO_COPY_PRESENT");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

// SUMMIT_SWAPCHAIN_TRACE=1: which frames arrive and when each buffer goes back.
static bool swapChainTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_SWAPCHAIN_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

Ref<AcceleratedBackingStore> AcceleratedBackingStore::create(WebPageProxy& webPage)
{
    return adoptRef(*new AcceleratedBackingStore(webPage));
}

AcceleratedBackingStore::AcceleratedBackingStore(WebPageProxy& webPage)
    : m_webPage(webPage)
{
}

AcceleratedBackingStore::~AcceleratedBackingStore()
{
    updateSurfaceID(0);
}

void AcceleratedBackingStore::updateSurfaceID(uint64_t surfaceID)
{
    if (m_surfaceID == surfaceID)
        return;

    if (m_surfaceID) {
        // The web process may be waiting on a frame this surface will never show.
        frameDone();
        storesByIdentifier().remove(std::exchange(m_storeIdentifier, 0));
        m_buffers.clear();
        m_images.clear();
        if (RefPtr process = m_legacyMainFrameProcess)
            process->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
        m_legacyMainFrameProcess = nullptr;
    }

    m_surfaceID = surfaceID;
    m_directFrameSeen = false;
    m_directPresentDeclined = false;
    if (!m_surfaceID)
        return;

    RefPtr webPage = m_webPage.get();
    if (!webPage)
        return;
    m_legacyMainFrameProcess = webPage->legacyMainFrameProcess();
    m_legacyMainFrameProcess->addMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID, *this);
    m_storeIdentifier = nextStoreIdentifier();
    storesByIdentifier().set(m_storeIdentifier, this);
    sendDirectTarget();
}

void AcceleratedBackingStore::setDirectTarget(std::shared_ptr<DirectPresentTargetHaiku> target)
{
    if (m_directTarget == target)
        return;
    m_directTarget = WTF::move(target);
    sendDirectTarget();
}

void AcceleratedBackingStore::sendDirectTarget()
{
    if (!m_surfaceID || !m_directTarget)
        return;
    m_directTargetSentAt = MonotonicTime::now();
    if (RefPtr process = m_legacyMainFrameProcess)
        process->send(Messages::AcceleratedSurface::SetDirectPresentTarget(m_directTarget->area()), m_surfaceID);
}

void AcceleratedBackingStore::requestDirectRedisplay()
{
    if (!m_surfaceID || !m_directTarget || !m_directTarget->enabled())
        return;
    if (RefPtr process = m_legacyMainFrameProcess)
        process->send(Messages::AcceleratedSurface::RedisplayDirect(), m_surfaceID);
}

void AcceleratedBackingStore::directFrame(uint64_t id)
{
    // Target 0: the web process cannot present directly (a Mesa without
    // Summit's present entry points); its frames all come through frame().
    if (!id) {
        m_directPresentDeclined = true;
        return;
    }
    m_directFrameSeen = true;
    m_directPresentDeclined = false;
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit backing store: surface=%llu direct frame=%llu\n",
            static_cast<unsigned long long>(m_surfaceID), static_cast<unsigned long long>(id));
    // Nothing to show here: the frame is on the screen already. The buffer
    // goes straight back, as a frame the view does not take would.
    RefPtr webPage = m_webPage.get();
    if (RefPtr client = webPage ? webPage->pageClient() : nullptr)
        static_cast<WebView&>(*client).presentedDirectlyHaiku();
    releaseBuffer(id);
    frameDone();
}

void AcceleratedBackingStore::didCreateSHMBuffer(uint64_t id, WebCore::ShareableBitmapHandle&& handle)
{
    // The web process sends a writable handle: app_server clones a bitmap's
    // area for writing, so only a writable mapping can be drawn in place.
    RefPtr bitmap = WebCore::ShareableBitmap::create(WTF::move(handle), WebCore::SharedMemory::Protection::ReadWrite);
    if (!bitmap)
        return;
    if (zeroCopyPresentEnabled() && bitmap->bytesPerRow() == static_cast<size_t>(bitmap->size().width()) * 4) {
        auto* data = bitmap->mutableSpan().data();
        area_id area = area_for(data);
        area_info info;
        if (area >= 0 && get_area_info(area, &info) == B_OK
            && set_area_protection(area, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA) == B_OK) {
            auto size = bitmap->size();
            auto image = std::make_shared<BBitmap>(area, static_cast<ptrdiff_t>(data - static_cast<uint8_t*>(info.address)),
                BRect(0, 0, size.width() - 1, size.height() - 1), 0, B_RGBA32, static_cast<int32>(bitmap->bytesPerRow()));
            if (image->InitCheck() == B_OK)
                m_images.set(id, WTF::move(image));
        }
    }
    m_buffers.add(id, WTF::move(bitmap));
}

void AcceleratedBackingStore::didDestroyBuffer(uint64_t id)
{
    m_buffers.remove(id);
    m_images.remove(id);
}

void AcceleratedBackingStore::releaseSharedBuffer(uint64_t storeIdentifier, uint64_t surfaceID, uint64_t id)
{
    auto* store = storesByIdentifier().get(storeIdentifier);
    // A store that has moved to another surface since does not own the buffer.
    bool owned = store && store->m_surfaceID == surfaceID && store->m_buffers.contains(id);
    if (swapChainTraceEnabled() && !owned)
        std::fprintf(stderr, "Summit backing store: surface=%llu release=%llu dropped (store %p)\n",
            static_cast<unsigned long long>(surfaceID), static_cast<unsigned long long>(id), store);
    if (owned)
        store->releaseBuffer(id);
}

void AcceleratedBackingStore::frame(uint64_t id, Rects&& damage, WTF::UnixFileDescriptor&&)
{
    static bool firstFrameTraced;
    if (!std::exchange(firstFrameTraced, true))
        SUMMIT_STARTUP_TRACE("first frame received by the browser");
    if (!m_directFrameSeen && !m_directPresentDeclined && m_directTarget && m_directTarget->enabled() && MonotonicTime::now() - m_directTargetSentAt >= 1_s) {
        sendDirectTarget();
        requestDirectRedisplay();
    }
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit backing store: surface=%llu frame=%llu mapped=%d image=%d\n",
            static_cast<unsigned long long>(m_surfaceID), static_cast<unsigned long long>(id), m_buffers.contains(id), m_images.contains(id));
    // The sync file descriptor orders GPU work between processes. Nothing is
    // shared here but memory the web process has finished writing.
    RefPtr bitmap = m_buffers.get(id);
    RefPtr webPage = m_webPage.get();
    RefPtr client = webPage ? webPage->pageClient() : nullptr;
    if (!bitmap || !client) {
        if (bitmap)
            releaseBuffer(id);
        frameDone();
        return;
    }

    // The buffer is in device pixels; the view wants its logical size back.
    float scale = webPage->deviceScaleFactor();
    auto size = bitmap->size();
    if (scale > 0 && scale != 1)
        size = WebCore::IntSize { static_cast<int>(std::lround(size.width() / scale)), static_cast<int>(std::lround(size.height() / scale)) };

    if (auto image = m_images.get(id)) {
        // The view draws this buffer until a newer frame replaces it; only then
        // may the web process paint into it again. The last reference can be
        // dropped on a window thread, so that release is posted back here.
        std::shared_ptr<const BBitmap> frame(image.get(), [image, bitmap, storeIdentifier = m_storeIdentifier, surfaceID = m_surfaceID, id](const BBitmap*) {
            // Usually a newer frame replaces this one right here, just before
            // FrameDone goes out: give the buffer back first. Posted, the
            // release waited behind every other task of a busy main thread,
            // and the web process, told to paint with no buffer free, made
            // another.
            if (RunLoop::isMain()) {
                releaseSharedBuffer(storeIdentifier, surfaceID, id);
                return;
            }
            RunLoop::mainSingleton().dispatch([storeIdentifier, surfaceID, id] {
                releaseSharedBuffer(storeIdentifier, surfaceID, id);
            });
        });
        // The damage is what differs from the previous frame, in device pixels;
        // none means the whole frame. A frame the view does not take (a hidden
        // tab) is released when this statement ends, also before FrameDone.
        // While the page is otherwise presented straight into the screen, a
        // frame drawn by app_server must be on the screen before the next one
        // goes there directly, or the older frame would cover the newer.
        if (m_directTarget && m_directTarget->enabled() && !m_directPresentDeclined) {
            static_cast<WebView&>(*client).presentSharedFrameHaiku(std::exchange(frame, nullptr), size, scale, damage,
                [protectedThis = Ref { *this }, surfaceID = m_surfaceID] {
                    if (protectedThis->m_surfaceID == surfaceID)
                        protectedThis->frameDone();
                });
            return;
        }
        static_cast<WebView&>(*client).presentSharedFrameHaiku(std::exchange(frame, nullptr), size, scale, damage);
        frameDone();
        return;
    }
    // presentBitmapHaiku's callback says when the view has drawn. The compositor
    // must not wait for that: it cannot paint another frame until FrameDone, and
    // a view that draws in response to a frame would then be waiting on itself.
    // Waiting is unnecessary anyway -- publish() copies the pixels into a BBitmap
    // before it returns, so the buffer is free the moment this call is done.
    client->presentBitmapHaiku(bitmap.releaseNonNull(), size, scale, [] { });
    releaseBuffer(id);
    frameDone();
}

// The buffer goes back to the surface's swap chain. Without this the chain
// never sees a free target and allocates another one for every frame, until
// the web process runs out of file descriptors and dies.
void AcceleratedBackingStore::releaseBuffer(uint64_t id)
{
    if (swapChainTraceEnabled())
        std::fprintf(stderr, "Summit backing store: surface=%llu release=%llu main=%d\n",
            static_cast<unsigned long long>(m_surfaceID), static_cast<unsigned long long>(id), RunLoop::isMain());
    if (RefPtr process = m_legacyMainFrameProcess)
        process->send(Messages::AcceleratedSurface::ReleaseBuffer(id, WTF::UnixFileDescriptor { }), m_surfaceID);
}

void AcceleratedBackingStore::frameDone()
{
    if (RefPtr process = m_legacyMainFrameProcess)
        process->send(Messages::AcceleratedSurface::FrameDone(), m_surfaceID);
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
