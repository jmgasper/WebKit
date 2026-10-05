/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "DrawingAreaProxyHaiku.h"

#include "BitmapFrameHaiku.h"
#include "DrawingAreaMessages.h"
#include "MessageSenderInlines.h"
#include "PageClient.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/ShareableBitmap.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(DrawingAreaProxyHaiku);

Ref<DrawingAreaProxyHaiku> DrawingAreaProxyHaiku::create(WebPageProxy& page, WebProcessProxy& process)
{
    return adoptRef(*new DrawingAreaProxyHaiku(page, process));
}

DrawingAreaProxyHaiku::DrawingAreaProxyHaiku(WebPageProxy& page, WebProcessProxy& process)
    : DrawingAreaProxy(page, process)
{
}

void DrawingAreaProxyHaiku::sizeDidChange()
{
    sendGeometry([] { });
}

void DrawingAreaProxyHaiku::deviceScaleFactorDidChange(CompletionHandler<void()>&& completionHandler)
{
    sendGeometry(WTF::move(completionHandler));
}

void DrawingAreaProxyHaiku::sendGeometry(CompletionHandler<void()>&& completionHandler)
{
    RefPtr page = this->page();
    if (!page || !page->hasRunningProcess()) {
        completionHandler();
        return;
    }
    ++m_geometryIdentifier;
    sendWithAsyncReply(Messages::DrawingArea::UpdateGeometryHaiku(m_geometryIdentifier, size(), page->deviceScaleFactor()), WTF::move(completionHandler));
}

void DrawingAreaProxyHaiku::updateBitmapHaiku(uint64_t geometryIdentifier, uint64_t frameIdentifier, const IntSize& viewSize, float scale, ShareableBitmapHandle&& handle)
{
    auto& connection = webProcessProxy().connection();
    MESSAGE_CHECK_BASE(frameIdentifier && frameIdentifier > m_lastFrameIdentifier, connection);
    MESSAGE_CHECK_BASE(geometryIdentifier <= m_geometryIdentifier, connection);
    // Verify the declared geometry before displaying bytes from WebContent.
    MESSAGE_CHECK_BASE(bitmapFrameHandleIsValidHaiku(viewSize, scale, handle), connection);
    auto bitmap = ShareableBitmap::create(WTF::move(handle), SharedMemory::Protection::ReadOnly);
    MESSAGE_CHECK_BASE(bitmap, connection);
    m_lastFrameIdentifier = frameIdentifier;
    RefPtr page = this->page();
    // A frame painted for an earlier geometry is still shown: the next one follows once the
    // web process has caught up. Dropping it left views that resize to fit their content
    // (extension popups) blank, because every frame was one geometry behind.
    if (!page) {
        send(Messages::DrawingArea::DidPresentBitmapHaiku(frameIdentifier));
        return;
    }
    RefPtr client = page->pageClient();
    if (!client) {
        send(Messages::DrawingArea::DidPresentBitmapHaiku(frameIdentifier));
        return;
    }
    client->presentBitmapHaiku(bitmap.releaseNonNull(), viewSize, scale, [weakThis = WeakPtr { *this }, frameIdentifier] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->send(Messages::DrawingArea::DidPresentBitmapHaiku(frameIdentifier));
    });
}

} // namespace WebKit
