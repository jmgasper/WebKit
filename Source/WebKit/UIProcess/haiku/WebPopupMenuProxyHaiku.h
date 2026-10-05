/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WebPopupMenuProxy.h"
#include <memory>
#include <wtf/Ref.h>

namespace WebKit {

class NativeSelectPopupHaiku;
struct WebViewStateHaiku;

class WebPopupMenuProxyHaiku final : public WebPopupMenuProxy {
public:
    static Ref<WebPopupMenuProxyHaiku> create(std::shared_ptr<WebViewStateHaiku> state, Client& client)
    {
        return adoptRef(*new WebPopupMenuProxyHaiku(std::move(state), client));
    }
    ~WebPopupMenuProxyHaiku() override;

    void showPopupMenu(const WebCore::IntRect&, WebCore::TextDirection, double pageScaleFactor, const Vector<WebPopupItem>&, const PlatformPopupMenuData&, int32_t selectedIndex) override;
    void hidePopupMenu() override;
    void cancelTracking() override;

private:
    WebPopupMenuProxyHaiku(std::shared_ptr<WebViewStateHaiku>, Client&);
    void didComplete(int32_t);

    struct CompletionState;
    const std::weak_ptr<WebViewStateHaiku> m_viewState;
    std::shared_ptr<CompletionState> m_completion;
    std::unique_ptr<NativeSelectPopupHaiku> m_native;
    bool m_completed { true };
};

} // namespace WebKit
