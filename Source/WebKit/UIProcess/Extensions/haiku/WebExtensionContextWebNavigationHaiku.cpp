/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "FrameTreeNodeData.h"
#include "WebExtensionFrameParameters.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include "WebPageProxy.h"

namespace WebKit {

// Traverse the process-provided tree in document order without recursive calls.
// Frame identifiers and parent relationships come from WebKit's frame metadata.
static Vector<WebExtensionFrameParameters> nativeFrameParameters(const FrameTreeNodeData& root, WebExtensionContext& context, WebExtensionTab& tab, std::optional<WebExtensionFrameIdentifier> target = std::nullopt)
{
    struct PendingFrame {
        const FrameTreeNodeData* node;
        WebExtensionFrameIdentifier parentIdentifier;
    };
    Vector<PendingFrame> pending { { &root, WebExtensionFrameConstants::NoneIdentifier } };
    Vector<WebExtensionFrameParameters> result;
    while (!pending.isEmpty()) {
        auto frame = pending.takeLast();
        auto& info = frame.node->info;
        auto identifier = toWebExtensionFrameIdentifier(info);
        if (!target || identifier == *target) {
            auto& url = info.request.url();
            WebExtensionFrameParameters parameters {
                .errorOccurred = info.errorOccurred,
                .url = context.hasPermission(url, &tab) ? std::optional { url } : std::nullopt,
                .parentFrameIdentifier = frame.parentIdentifier,
                .frameIdentifier = target ? std::nullopt : std::optional { identifier },
                .documentIdentifier = info.documentID ? Markable<WTF::UUID> { info.documentID->object() } : Markable<WTF::UUID> { }
            };
            result.append(WTF::move(parameters));
            if (target)
                break;
        }
        for (size_t i = frame.node->children.size(); i; --i)
            pending.append({ &frame.node->children[i - 1], identifier });
    }
    return result;
}

void WebExtensionContext::webNavigationGetFrame(WebExtensionTabIdentifier tabIdentifier, WebExtensionFrameIdentifier frameIdentifier, CompletionHandler<void(std::expected<std::optional<WebExtensionFrameParameters>, WebExtensionError>&&)>&& completionHandler)
{
    if (!isLoaded() || !hasPermission(WebExtensionPermission::webNavigation())) {
        completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "permission denied"_s));
        return;
    }
    RefPtr tab = getTab(tabIdentifier);
    RefPtr page = tab ? tab->nativePage() : nullptr;
    if (!page) {
        completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "tab not found"_s));
        return;
    }
    page->getAllFrames([protectedThis = Ref { *this }, tab = tab.releaseNonNull(), page = Ref { *page }, frameIdentifier, completionHandler = WTF::move(completionHandler)](std::optional<FrameTreeNodeData>&& tree) mutable {
        if (!protectedThis->isLoaded() || !protectedThis->hasPermission(WebExtensionPermission::webNavigation())) {
            completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "permission denied"_s));
            return;
        }
        if (!tab->extensionHasAccess() || protectedThis->getTab(tab->identifier()) != tab.ptr() || tab->nativePage() != page.ptr()) {
            completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "tab not found"_s));
            return;
        }
        if (!tree || !tree->info.isMainFrame) {
            completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "main frame not found"_s));
            return;
        }
        auto frames = nativeFrameParameters(*tree, protectedThis, tab, frameIdentifier);
        if (frames.isEmpty())
            completionHandler(toWebExtensionError("webNavigation.getFrame()"_s, nullString(), "frame not found"_s));
        else
            completionHandler(std::optional { frames.takeLast() });
    });
}

void WebExtensionContext::webNavigationGetAllFrames(WebExtensionTabIdentifier tabIdentifier, CompletionHandler<void(std::expected<Vector<WebExtensionFrameParameters>, WebExtensionError>&&)>&& completionHandler)
{
    if (!isLoaded() || !hasPermission(WebExtensionPermission::webNavigation())) {
        completionHandler(toWebExtensionError("webNavigation.getAllFrames()"_s, nullString(), "permission denied"_s));
        return;
    }
    RefPtr tab = getTab(tabIdentifier);
    RefPtr page = tab ? tab->nativePage() : nullptr;
    if (!page) {
        completionHandler(toWebExtensionError("webNavigation.getAllFrames()"_s, nullString(), "tab not found"_s));
        return;
    }
    page->getAllFrames([protectedThis = Ref { *this }, tab = tab.releaseNonNull(), page = Ref { *page }, completionHandler = WTF::move(completionHandler)](std::optional<FrameTreeNodeData>&& tree) mutable {
        if (!protectedThis->isLoaded() || !protectedThis->hasPermission(WebExtensionPermission::webNavigation())) {
            completionHandler(toWebExtensionError("webNavigation.getAllFrames()"_s, nullString(), "permission denied"_s));
            return;
        }
        if (!tab->extensionHasAccess() || protectedThis->getTab(tab->identifier()) != tab.ptr() || tab->nativePage() != page.ptr()) {
            completionHandler(toWebExtensionError("webNavigation.getAllFrames()"_s, nullString(), "tab not found"_s));
            return;
        }
        if (!tree || !tree->info.isMainFrame) {
            completionHandler(toWebExtensionError("webNavigation.getAllFrames()"_s, nullString(), "main frame not found"_s));
            return;
        }
        completionHandler(nativeFrameParameters(*tree, protectedThis, tab));
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
