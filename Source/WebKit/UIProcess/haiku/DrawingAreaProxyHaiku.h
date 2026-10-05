/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "DrawingAreaProxy.h"
#include <wtf/RefCounted.h>

namespace WebKit {

class DrawingAreaProxyHaiku final : public DrawingAreaProxy, public RefCounted<DrawingAreaProxyHaiku> {
    WTF_MAKE_TZONE_ALLOCATED(DrawingAreaProxyHaiku);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(DrawingAreaProxyHaiku);
public:
    static Ref<DrawingAreaProxyHaiku> create(WebPageProxy&, WebProcessProxy&);
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    DrawingAreaProxyHaiku(WebPageProxy&, WebProcessProxy&);
    void sizeDidChange() override;
    void deviceScaleFactorDidChange(CompletionHandler<void()>&&) override;
    void updateBitmapHaiku(uint64_t, uint64_t, const WebCore::IntSize&, float, WebCore::ShareableBitmapHandle&&) override;
    void sendGeometry(CompletionHandler<void()>&&);
    uint64_t m_geometryIdentifier { 0 };
    uint64_t m_lastFrameIdentifier { 0 };
};

} // namespace WebKit
