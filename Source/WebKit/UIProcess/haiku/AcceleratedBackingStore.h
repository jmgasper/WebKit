/*
 * Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)

#include "MessageReceiver.h"
#include <WebCore/IntRect.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>
#include <memory>

class BBitmap;

namespace WebKit {

class DirectPresentTargetHaiku;
class WebPageProxy;
class WebProcessProxy;

// The UI process end of AcceleratedSurface. The web process paints a frame into
// a ShareableBitmap, says which buffer it used, and waits for FrameDone before
// painting the next one. The view draws the buffer in place (see m_images), or,
// if app_server cannot map it, a copy made by presentBitmapHaiku.
//
// Only the shared memory buffer messages are answered. The DMA-BUF and Android
// ones belong to platforms that can pass GPU buffers between processes.
class AcceleratedBackingStore final : public IPC::MessageReceiver, public RefCounted<AcceleratedBackingStore> {
    WTF_MAKE_TZONE_ALLOCATED(AcceleratedBackingStore);
    WTF_MAKE_NONCOPYABLE(AcceleratedBackingStore);
public:
    using Rects = Vector<WebCore::IntRect, 1>;

    static Ref<AcceleratedBackingStore> create(WebPageProxy&);
    ~AcceleratedBackingStore();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    // A surface is made for each size the page takes, and each has its own
    // identifier to receive messages on. Zero detaches from the current one.
    void updateSurfaceID(uint64_t);
    // Where on the screen the web process may put frames itself
    // (DirectPresentHaiku.h); passed to each surface. Null: nowhere.
    void setDirectTarget(std::shared_ptr<DirectPresentTargetHaiku>);
    // The page is wanted on the screen again in full: its place changed, or
    // app_server drew over it.
    void requestDirectRedisplay();

private:
    explicit AcceleratedBackingStore(WebPageProxy&);

    // IPC::MessageReceiver; the body is generated from
    // UIProcess/glib/AcceleratedBackingStore.messages.in.
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    void didCreateSHMBuffer(uint64_t id, WebCore::ShareableBitmapHandle&&);
    void didDestroyBuffer(uint64_t id);
    void frame(uint64_t id, Rects&&, WTF::UnixFileDescriptor&&);
    // A frame the web process copied into the screen itself.
    void directFrame(uint64_t id);
    void sendDirectTarget();
    void releaseBuffer(uint64_t id);
    void frameDone();
    static void releaseSharedBuffer(uint64_t storeIdentifier, uint64_t surfaceID, uint64_t id);

    WeakPtr<WebPageProxy> m_webPage;
    RefPtr<WebProcessProxy> m_legacyMainFrameProcess;
    uint64_t m_surfaceID { 0 };
    // Unique in this process, unlike m_surfaceID; zero while detached.
    uint64_t m_storeIdentifier { 0 };
    HashMap<uint64_t, RefPtr<WebCore::ShareableBitmap>> m_buffers;
    // Each buffer's memory as a BBitmap that app_server draws from directly,
    // so a frame is never copied in this process. The view keeps the frame it
    // shows; the buffer returns to the web process once a newer one replaces it.
    HashMap<uint64_t, std::shared_ptr<BBitmap>> m_images;
    std::shared_ptr<DirectPresentTargetHaiku> m_directTarget;
    // A target sent when the surface is announced can arrive before the web
    // process listens: while the surface sends ordinary frames although it
    // has a place on the screen, the target goes again, once a second at most.
    bool m_directFrameSeen { false };
    // The web process said it cannot present into the screen (DirectFrame 0).
    bool m_directPresentDeclined { false };
    MonotonicTime m_directTargetSentAt;
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
