/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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
#include "MessageSenderInlines.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionMessageTargetParameters.h"
#include "WebExtensionPermission.h"
#include "WebExtensionTabQueryParameters.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionTab.h"
#include "WebExtensionWindow.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include <Bitmap.h>
#include <WebCore/UserContentURLPattern.h>
#include <wtf/Box.h>
#include <wtf/CallbackAggregator.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/Base64.h>
#include <cstring>
#include <limits>

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPixmap.h>
#include <skia/core/SkStream.h>
#include <skia/encode/SkJpegEncoder.h>
#include <skia/encode/SkPngEncoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#else
#include <BitmapStream.h>
#include <DataIO.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#endif

namespace WebKit {
void WebExtensionContext::tabsSendMessage(WebExtensionTabIdentifier identifier, const String& messageJSON, const WebExtensionMessageTargetParameters& target, const WebExtensionMessageSenderParameters& sender, bool userGesture, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completion)
{
    static constexpr auto apiName = "tabs.sendMessage()"_s;
    RefPtr tab = getTab(identifier);
    if (!tab || !tab->isOpen()) {
        completion(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }
    RefPtr page = tab->nativePage();
    if (!page) {
        completion(toWebExtensionError(apiName, nullString(), "the receiving document is unavailable"_s));
        return;
    }
    auto world = isURLForThisExtension(tab->url()) ? WebExtensionContentWorldType::Main : WebExtensionContentWorldType::ContentScript;
    auto targets = processes({ WebExtensionEventListenerType::RuntimeOnMessage }, { world }, [&](auto&, auto& listenerPage, auto&) {
        return listenerPage.identifier() == page->identifier();
    });
    auto noReceiver = toWebExtensionError(apiName, nullString(), "could not establish connection; receiving end does not exist"_s);
    if (targets.isEmpty()) {
        completion(WTF::move(noReceiver));
        return;
    }
    auto scopedTarget = target;
    scopedTarget.pageProxyIdentifier = page->identifier();
    // The target remains in this extension's namespace and is checked again
    // against the actual frame and document in each receiving web process.
    auto hadListener = Box<bool>::create(false);
    Ref aggregator = EagerCallbackAggregator<void(String)>::create(
        [hadListener, completion = WTF::move(completion), noReceiver = WTF::move(noReceiver)](String&& reply) mutable {
            if (reply.isNull() && !*hadListener) {
                completion(WTF::move(noReceiver));
                return;
            }
            completion(WTF::move(reply));
        }, String { });
    for (Ref process : targets) {
        process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchTabsMessageEvent(world, messageJSON, scopedTarget, sender, userGesture),
            [aggregator, hadListener](String&& reply, bool delivered) {
                *hadListener |= delivered;
                if (!reply.isNull())
                    aggregator.get()(WTF::move(reply));
            }, this->identifier());
    }
}

void WebExtensionContext::tabsGet(WebExtensionTabIdentifier identifier, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&& completion)
{
    RefPtr tab = getTab(identifier);
    if (!tab || !tab->isOpen()) {
        completion(toWebExtensionError("tabs.get()"_s, nullString(), "tab not found"_s));
        return;
    }
    completion({ tab->parameters() });
}

void WebExtensionContext::tabsGetCurrent(WebPageProxyIdentifier page, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&& completion)
{
    RefPtr tab = getCurrentTab(page, IncludeExtensionViews::No);
    // Background pages and unassociated extension views are not browser tabs.
    if (!tab || !tab->isOpen()) {
        completion({ std::nullopt });
        return;
    }
    completion({ tab->parameters() });
}

void WebExtensionContext::tabsQuery(WebPageProxyIdentifier page, const WebExtensionTabQueryParameters& query, CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)>&& completion)
{
    if ((query.index && *query.index > 9007199254740991ULL)
        || (query.windowIdentifier && !isValid(query.windowIdentifier))
        || (query.titlePattern && query.titlePattern->contains('\0'))) {
        completion(toWebExtensionError("tabs.query()"_s, "info"_s, "invalid query parameters"_s));
        return;
    }
    MatchPatternSet urlPatterns;
    if (query.urlPatterns) {
        for (auto& source : *query.urlPatterns) {
            RefPtr pattern = source.contains('\0') ? nullptr : WebExtensionMatchPattern::getOrCreate(source);
            if (!pattern || !pattern->isSupported()) {
                completion(toWebExtensionError("tabs.query()"_s, "url"_s, "unsupported or invalid match pattern"_s));
                return;
            }
            urlPatterns.add(pattern.releaseNonNull());
        }
    }
    RefPtr currentWindow = getWindow(WebExtensionWindowConstants::CurrentIdentifier, page);
    Vector<WebExtensionTabParameters> result;
    for (Ref window : openWindows()) {
        if (!window->isOpen() || !window->extensionHasAccess())
            continue;
        bool isCurrentWindow = currentWindow && currentWindow->identifier() == window->identifier();
        if (query.windowIdentifier) {
            if (isCurrent(query.windowIdentifier) ? !isCurrentWindow : window->identifier() != *query.windowIdentifier)
                continue;
        }
        if (query.currentWindow && *query.currentWindow != isCurrentWindow)
            continue;
        if (query.frontmostWindow && *query.frontmostWindow != window->isFrontmost())
            continue;
        // The registered native browser currently creates normal windows.
        if (query.windowType && !query.windowType->contains(WebExtensionWindow::TypeFilter::Normal))
            continue;
        for (Ref tab : window->tabs()) {
            if (!tab->isOpen() || !tab->extensionHasAccess())
                continue;
            RefPtr nativePage = tab->nativePage();
            if (!nativePage)
                continue;
            bool active = tab->isActive();
            if ((query.index && *query.index != tab->index())
                || (query.active && *query.active != active)
                || (query.selected && *query.selected != tab->isSelected())
                || (query.hidden && *query.hidden == active)
                || (query.loading && *query.loading == tab->isLoadingComplete())
                || (query.audible && *query.audible != nativePage->isPlayingAudio())
                || (query.discarded && *query.discarded != tab->isDiscarded())
                || (query.muted && *query.muted != nativePage->isAudioMuted())
                || (query.pinned && *query.pinned))
                continue;
            // The tabs permission exposes metadata, not website execution or
            // network access. Keep that separate from the host access check.
            if ((query.urlPatterns || query.titlePattern)
                && !hasPermission(WebExtensionPermission::tabs(), tab.ptr()) && !tab->extensionHasPermission())
                continue;
            if (query.urlPatterns) {
                bool matches = false;
                OptionSet<WebExtensionMatchPattern::Options> options;
                if (hasAccessToFileURLs())
                    options.add(WebExtensionMatchPattern::Options::AllowFileScheme);
                for (auto& pattern : urlPatterns) {
                    if (pattern->matchesURL(tab->url(), options)) {
                        matches = true;
                        break;
                    }
                }
                if (!matches)
                    continue;
            }
            if (query.titlePattern && !WebCore::matchesWildcardPattern(*query.titlePattern, tab->title()))
                continue;
            result.append(tab->parameters());
        }
    }
    completion(WTF::move(result));
}
// The picture is the frame the tab's view is showing: the page as composited,
// in device pixels (twice the CSS size on a 200% screen, as in other browsers).
void WebExtensionContext::tabsCaptureVisibleTab(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionWindowIdentifier> windowIdentifier, WebExtensionTab::ImageFormat imageFormat, uint8_t imageQuality, CompletionHandler<void(std::expected<URL, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.captureVisibleTab()"_s;
    RefPtr window = getWindow(windowIdentifier.value_or(WebExtensionWindowConstants::CurrentIdentifier), webPageProxyIdentifier);
    if (!window) {
        completionHandler(toWebExtensionError(apiName, nullString(), "window not found"_s));
        return;
    }
    RefPtr activeTab = window->activeTab();
    if (!activeTab) {
        completionHandler(toWebExtensionError(apiName, nullString(), "active tab not found"_s));
        return;
    }
    if (!activeTab->extensionHasPermission()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "either the 'activeTab' permission or granted host permissions for the current website are required"_s));
        return;
    }
    RefPtr page = activeTab->nativePage();
    RefPtr client = page ? page->pageClient() : nullptr;
    auto frame = client ? static_cast<WebView&>(*client).presentedFrameHaiku() : nullptr;
    if (!frame || !frame->image) {
        completionHandler(URL { });
        return;
    }
    // Copy the pixels now: the frame's buffer belongs to the web process's
    // swap chain and goes back to it once a newer frame replaces this one.
    auto bounds = frame->image->Bounds();
    int width = bounds.IntegerWidth() + 1;
    int height = bounds.IntegerHeight() + 1;
    size_t rowBytes = frame->image->BytesPerRow();
    Vector<uint8_t> pixels(rowBytes * height);
    std::memcpy(pixels.mutableSpan().data(), frame->image->Bits(), pixels.size());
    frame = nullptr;

    // Encoding several megapixels takes long enough to stall the browser.
    static NeverDestroyed<Ref<WorkQueue>> queue { WorkQueue::create("org.webkit.haiku.tabsCaptureVisibleTab"_s) };
    queue.get()->dispatch([pixels = WTF::move(pixels), width, height, rowBytes, imageFormat, imageQuality, completionHandler = WTF::move(completionHandler)]() mutable {
#if USE(SKIA)
        SkPixmap pixmap(SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType), pixels.span().data(), rowBytes);
        SkDynamicMemoryWStream stream;
        bool encoded = false;
        if (imageFormat == WebExtensionTab::ImageFormat::JPEG) {
            SkJpegEncoder::Options options;
            options.fQuality = imageQuality;
            encoded = SkJpegEncoder::Encode(&stream, pixmap, options);
        } else
            encoded = SkPngEncoder::Encode(&stream, pixmap, { });
        String dataURL;
        if (encoded) {
            auto data = stream.detachAsData();
            auto base64 = base64EncodeToString(std::span { static_cast<const uint8_t*>(data->data()), data->size() });
            dataURL = makeString("data:"_s, imageFormat == WebExtensionTab::ImageFormat::JPEG ? "image/jpeg"_s : "image/png"_s, ";base64,"_s, base64);
        }
#else
        // Without Skia (the app_server backend) the Translation Kit encodes, as
        // ImageBufferHaiku's platformEncodeData does; its JPEG translator keeps
        // its own quality setting.
        UNUSED_PARAM(imageQuality);
        auto* bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
        size_t copyBytes = std::min<size_t>(rowBytes, bitmap->BytesPerRow());
        for (int row = 0; row < height; ++row)
            std::memcpy(static_cast<uint8_t*>(bitmap->Bits()) + row * bitmap->BytesPerRow(), pixels.span().data() + row * rowBytes, copyBytes);
        BBitmapStream input(bitmap);
        BMallocIO output;
        bool encoded = BTranslatorRoster::Default()->Translate(&input, nullptr, nullptr, &output,
            imageFormat == WebExtensionTab::ImageFormat::JPEG ? B_JPEG_FORMAT : B_PNG_FORMAT) == B_OK;
        String dataURL;
        if (encoded && output.BufferLength()) {
            auto base64 = base64EncodeToString(std::span { static_cast<const uint8_t*>(output.Buffer()), output.BufferLength() });
            dataURL = makeString("data:"_s, imageFormat == WebExtensionTab::ImageFormat::JPEG ? "image/jpeg"_s : "image/png"_s, ";base64,"_s, base64);
        }
#endif
        RunLoop::mainSingleton().dispatch([dataURL = WTF::move(dataURL).isolatedCopy(), completionHandler = WTF::move(completionHandler)]() mutable {
            if (dataURL.isEmpty()) {
                completionHandler(toWebExtensionError(apiName, nullString(), "the picture could not be encoded"_s));
                return;
            }
            completionHandler(URL { dataURL });
        });
    });
}
// Summit moves tabs within a window; moving one to another window would mean
// moving its page view between native windows, which the browser cannot do yet.
void WebExtensionContext::tabsMove(Vector<WebExtensionTabIdentifier> tabIdentifiers, std::optional<WebExtensionWindowIdentifier> windowIdentifier, double targetIndex, CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.move()"_s;
    Vector<Ref<WebExtensionTab>> tabs;
    for (auto& identifier : tabIdentifiers) {
        RefPtr tab = getTab(identifier);
        if (!tab || !tab->isOpen()) {
            completionHandler(toWebExtensionError(apiName, nullString(), makeString("tab '"_s, identifier.toUInt64(), "' was not found"_s)));
            return;
        }
        tabs.append(tab.releaseNonNull());
    }
    RefPtr window = windowIdentifier ? getWindow(*windowIdentifier) : nullptr;
    if (windowIdentifier && !window) {
        completionHandler(toWebExtensionError(apiName, nullString(), "window not found"_s));
        return;
    }
    for (auto& tab : tabs) {
        if (window && tab->window() != window) {
            completionHandler(toWebExtensionError(apiName, nullString(), "moving tabs to another window is not supported"_s));
            return;
        }
    }
    if (!std::isfinite(targetIndex) || targetIndex < -1 || targetIndex > std::numeric_limits<int32_t>::max()) {
        completionHandler(toWebExtensionError(apiName, "index"_s, "it must be -1 or a tab position"_s));
        return;
    }
    // One after another, each to the position after the previous one, as the
    // API describes; -1 keeps appending.
    class TabMover final : public RefCounted<TabMover> {
    public:
        TabMover(Vector<Ref<WebExtensionTab>>&& tabs, double targetIndex, CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)>&& completion)
            : m_tabs(WTF::move(tabs)), m_targetIndex(targetIndex), m_completion(WTF::move(completion)) { }

        void move(size_t position)
        {
            if (position == m_tabs.size()) {
                Vector<WebExtensionTabParameters> result;
                for (auto& tab : m_tabs)
                    result.append(tab->parameters());
                m_completion(WTF::move(result));
                return;
            }
            int32_t index = m_targetIndex < 0 ? -1 : static_cast<int32_t>(m_targetIndex) + static_cast<int32_t>(position);
            m_tabs[position]->moveToIndex(index, [protectedThis = Ref { *this }, position](std::expected<void, WebExtensionError>&& result) {
                if (!result) {
                    protectedThis->m_completion(makeUnexpected(result.error()));
                    return;
                }
                protectedThis->move(position + 1);
            });
        }

    private:
        Vector<Ref<WebExtensionTab>> m_tabs;
        double m_targetIndex;
        CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)> m_completion;
    };
    adoptRef(*new TabMover(WTF::move(tabs), targetIndex, WTF::move(completionHandler)))->move(0);
}

}
#endif
