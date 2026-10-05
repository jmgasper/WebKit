/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebPopupMenuProxyHaiku.h"

#include "NativeSelectPopupHaiku.h"
#include "WebPopupItem.h"
#include "WebViewStateHaiku.h"
#include <WebCore/IntRect.h>
#include <cmath>
#include <wtf/Function.h>
#include <wtf/RunLoop.h>
#include <wtf/text/CString.h>

namespace WebKit {

struct WebPopupMenuProxyHaiku::CompletionState {
    // Accessed only by application-thread callbacks. Native workers retain the
    // state but never inspect the function or reference the proxy/client.
    Function<void(int32_t)> callback;
    std::atomic<bool> cancelled { false };
};

WebPopupMenuProxyHaiku::WebPopupMenuProxyHaiku(std::shared_ptr<WebViewStateHaiku> state, Client& client)
    : WebPopupMenuProxy(client), m_viewState(std::move(state))
{
    ASSERT(RunLoop::isMain());
}

WebPopupMenuProxyHaiku::~WebPopupMenuProxyHaiku()
{
    ASSERT(RunLoop::isMain());
    if (m_completion) {
        m_completion->cancelled = true;
        m_completion->callback = nullptr;
    }
    // The native owner cancels and releases immediately; its worker owns all
    // remaining menu data until tracking has finished.
}

void WebPopupMenuProxyHaiku::showPopupMenu(const WebCore::IntRect& rect, WebCore::TextDirection direction, double pageScaleFactor, const Vector<WebPopupItem>& items, const PlatformPopupMenuData&, int32_t selectedIndex)
{
    ASSERT(RunLoop::isMain());
    hidePopupMenu();
    m_completed = false;
    auto state = m_viewState.lock();
    if (!state || state->closed || items.isEmpty() || !std::isfinite(pageScaleFactor) || pageScaleFactor <= 0) {
        didComplete(-1);
        return;
    }
    WebViewGeometryHaiku geometry;
    {
        std::lock_guard lock(state->mutex);
        geometry = state->geometry;
    }
    if (!geometry.attached || !geometry.visible) {
        didComplete(-1);
        return;
    }

    // Haiku uses the desktop RenderListBox for multiple/size>1 selects. This
    // upstream popup interface carries one selected list index, not a set.
    std::vector<NativeSelectPopupHaiku::Item> nativeItems;
    nativeItems.reserve(items.size());
    for (const auto& item : items) {
        auto text = item.m_text.utf8();
        std::string label;
        if (text.length())
            label.assign(text.legacyCStringPointer(), text.length());
        nativeItems.push_back({ std::move(label), item.m_isEnabled, item.m_isLabel, item.m_type == WebPopupItem::Type::Separator });
    }
    BRect anchor(geometry.screenOrigin.x() + rect.x() * pageScaleFactor,
        geometry.screenOrigin.y() + rect.y() * pageScaleFactor,
        geometry.screenOrigin.x() + (static_cast<double>(rect.x()) + rect.width()) * pageScaleFactor - 1,
        geometry.screenOrigin.y() + (static_cast<double>(rect.y()) + rect.height()) * pageScaleFactor - 1);
    if (!std::isfinite(anchor.left) || !std::isfinite(anchor.top)
        || !std::isfinite(anchor.right) || !std::isfinite(anchor.bottom) || !anchor.IsValid()) {
        didComplete(-1);
        return;
    }

    m_completion = std::make_shared<CompletionState>();
    m_completion->callback = [this](int32_t index) { didComplete(index); };
    m_native = std::make_unique<NativeSelectPopupHaiku>(std::move(nativeItems), anchor, direction == WebCore::TextDirection::RTL, selectedIndex,
        [completion = m_completion](int32_t index) {
            if (completion->cancelled)
                return;
            RunLoop::mainSingleton().dispatch([completion, index] {
                if (auto callback = std::exchange(completion->callback, nullptr))
                    callback(index);
            });
        });
    if (m_native->start() != B_OK)
        didComplete(-1);
}

void WebPopupMenuProxyHaiku::didComplete(int32_t index)
{
    ASSERT(RunLoop::isMain());
    if (std::exchange(m_completed, true))
        return;
    Ref protectedThis { *this };
    if (m_completion) {
        m_completion->cancelled = true;
        m_completion->callback = nullptr;
    }
    m_native.reset();
    auto state = m_viewState.lock();
    if (!state || state->closed)
        return;
    if (CheckedPtr client = this->client())
        client->valueChangedForPopupMenu(this, index);
}

void WebPopupMenuProxyHaiku::hidePopupMenu()
{
    ASSERT(RunLoop::isMain());
    didComplete(-1);
}

void WebPopupMenuProxyHaiku::cancelTracking()
{
    ASSERT(RunLoop::isMain());
    invalidate();
    hidePopupMenu();
}

} // namespace WebKit
