/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionUtilities.h"
namespace WebKit {
static bool matchesWindow(const WebExtensionWindow* window, OptionSet<WebExtensionWindowTypeFilter> filter)
{
    // The native host registry currently contains normal browser windows.
    return window && window->isOpen() && window->extensionHasAccess() && filter.contains(WebExtensionWindowTypeFilter::Normal);
}
void WebExtensionContext::windowsGet(WebPageProxyIdentifier page, WebExtensionWindowIdentifier identifier, OptionSet<WebExtensionWindowTypeFilter> filter, WebExtensionWindow::PopulateTabs populate, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&& completion)
{
    RefPtr window = getWindow(identifier, page);
    if (!matchesWindow(window.get(), filter)) {
        completion(toWebExtensionError("windows.get()"_s, nullString(), "window not found or does not match windowTypes"_s));
        return;
    }
    completion(window->parameters(populate));
}
void WebExtensionContext::windowsGetLastFocused(OptionSet<WebExtensionWindowTypeFilter> filter, WebExtensionWindow::PopulateTabs populate, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&& completion)
{
    RefPtr window = frontmostWindow();
    if (!matchesWindow(window.get(), filter)) {
        completion(toWebExtensionError("windows.getLastFocused()"_s, nullString(), "window not found or does not match windowTypes"_s));
        return;
    }
    completion(window->parameters(populate));
}
void WebExtensionContext::windowsGetAll(OptionSet<WebExtensionWindowTypeFilter> filter, WebExtensionWindow::PopulateTabs populate, CompletionHandler<void(std::expected<Vector<WebExtensionWindowParameters>, WebExtensionError>&&)>&& completion)
{
    Vector<WebExtensionWindowParameters> result;
    for (Ref window : openWindows()) {
        if (matchesWindow(window.ptr(), filter))
            result.append(window->parameters(populate));
    }
    completion(WTF::move(result));
}
}
#endif
