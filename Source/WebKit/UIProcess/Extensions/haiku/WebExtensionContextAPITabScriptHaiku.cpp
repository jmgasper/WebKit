/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIContentWorld.h"
#include "FrameTreeNodeData.h"
#include "MessageSenderInlines.h"
#include "WebExtensionConstants.h"
#include "WebExtensionTabScriptExecution.h"
#include "WebPageMessages.h"
#include "WebPageProxy.h"
#include <wtf/Box.h>
#include <wtf/CallbackAggregator.h>

namespace WebKit {

void WebExtensionContext::tabsExecuteScriptNative(WebPageProxyIdentifier callingPage, std::optional<WebExtensionTabIdentifier> identifier, const WebExtensionTabScriptParameters& parameters, bool userGesture, CompletionHandler<void(std::expected<Vector<RefPtr<WebCore::SerializedScriptValue>>, WebExtensionError>&&)>&& completion)
{
    constexpr auto apiName = "tabs.executeScript()"_s;
    RefPtr<WebExtensionTab> tab;
    if (identifier)
        tab = getTab(*identifier);
    else if (RefPtr window = getWindow(WebExtensionWindowConstants::CurrentIdentifier, callingPage))
        tab = window->activeTab();
    RefPtr page = tab ? tab->nativePage() : nullptr;
    if (!tab || !page || !tab->isOpen() || !tab->extensionHasAccess() || !safeToInjectContent() || !m_contentScriptWorld || extension().supportsManifestVersion(3)) {
        completion(toWebExtensionError(apiName, nullString(), "the target tab or extension is unavailable"_s));
        return;
    }
    if (parameters.code.has_value() == parameters.file.has_value()) {
        completion(toWebExtensionError(apiName, nullString(), "exactly one code or file source is required"_s));
        return;
    }
    String source;
    URL sourceURL;
    if (parameters.code)
        source = *parameters.code;
    else {
        sourceURL = URL { baseURL(), *parameters.file };
        if (!isURLForThisExtension(sourceURL)) {
            completion(toWebExtensionError(apiName, "file"_s, "the script must be packaged with this extension"_s));
            return;
        }
        auto resource = extension().resourceStringForPath(*parameters.file, WebExtension::CacheResult::Yes);
        if (!resource) {
            completion(toWebExtensionError(apiName, "file"_s, "the script resource could not be loaded"_s));
            return;
        }
        source = WTF::move(*resource);
    }
    if (source.length() > webExtensionMaxMessageLength) {
        completion(toWebExtensionError(apiName, nullString(), "the script exceeds the maximum size"_s));
        return;
    }
    Ref world = *m_contentScriptWorld;
    auto stillCurrent = [protectedThis = Ref { *this }, tab, page, world] {
        return protectedThis->safeToInjectContent() && protectedThis->m_contentScriptWorld == world.ptr()
            && tab->isOpen() && tab->extensionHasAccess() && tab->nativePage() == page;
    };
    page->getAllFrames([this, protectedThis = Ref { *this }, tab, page, world, parameters, source, sourceURL, userGesture, stillCurrent, completion = WTF::move(completion)](std::optional<FrameTreeNodeData>&& frames) mutable {
        if (!frames || !stillCurrent()) {
            completion(toWebExtensionError("tabs.executeScript()"_s, nullString(), "the target document is unavailable"_s));
            return;
        }
        auto captured = captureWebExtensionTabScriptTargets(*frames, page->identifier(), parameters);
        if (!captured) {
            completion(toWebExtensionError("tabs.executeScript()"_s, nullString(), captured.error()));
            return;
        }
        Vector<WebExtensionTabScriptTarget> targets;
        for (const auto& target : *captured) {
            auto url = webExtensionTabScriptPermissionURL(target, parameters.matchAboutBlank);
            if (!url || (url->protocolIs("file"_s) && !hasAccessToFileURLs())) {
                // MV2 allFrames keeps successful child results, but a failure
                // in the requested root frame rejects the operation.
                if (targets.isEmpty()) {
                    completion(toWebExtensionError("tabs.executeScript()"_s, nullString(), "this extension cannot inject into the requested document"_s));
                    return;
                }
                continue;
            }
            targets.append(target);
        }
        // Use host or activeTab grants already approved by the native browser.
        // The tabs metadata permission must not authorize script execution.
        auto results = Box<Vector<WebExtensionTabScriptExecutionResult>>::create();
        results->resize(targets.size());
        Ref aggregator = MainRunLoopCallbackAggregator::create([results, completion = WTF::move(completion)]() mutable {
            if (results->isEmpty() || !results->first() || !*results->first()) {
                String error = !results->isEmpty() && !results->first() ? results->first().error() : "the requested frame did not execute the script"_s;
                completion(toWebExtensionError("tabs.executeScript()"_s, nullString(), error));
                return;
            }
            Vector<RefPtr<WebCore::SerializedScriptValue>> values;
            for (auto& result : *results) {
                if (result && *result)
                    values.append(WTF::move(*result));
            }
            completion(WTF::move(values));
        });
        for (size_t index = 0; index < targets.size(); ++index) {
            auto target = targets[index];
            auto url = webExtensionTabScriptPermissionURL(target, parameters.matchAboutBlank);
            if (!url || !hasPermission(*url, tab.get(), { })) {
                (*results)[index] = makeUnexpected("this extension does not have host access to the frame"_s);
                continue;
            }
            auto frameIdentifier = target.documents.last().frameIdentifier;
            Ref process = page->processContainingFrame(frameIdentifier);
            process->sendWithAsyncReply(Messages::WebPage::WaitForExtensionTabScript(this->identifier(), world->identifier(), target, parameters.runAt),
                [this, protectedThis, tab, page, world, parameters, source, sourceURL, userGesture, stillCurrent, frameIdentifier, results, index, aggregator](std::expected<WebExtensionTabScriptTarget, String>&& ready) mutable {
                    if (!ready || !stillCurrent()) {
                        (*results)[index] = makeUnexpected(ready ? "the extension or tab changed while waiting to inject"_s : ready.error());
                        return;
                    }
                    auto url = webExtensionTabScriptPermissionURL(*ready, parameters.matchAboutBlank);
                    // Recheck after document readiness; a grant may expire
                    // or be revoked while the request waits for resources.
                    if (!url || (url->protocolIs("file"_s) && !hasAccessToFileURLs()) || !hasPermission(*url, tab.get(), { })) {
                        (*results)[index] = makeUnexpected("host access was lost before script execution"_s);
                        return;
                    }
                    Ref process = page->processContainingFrame(frameIdentifier);
                    auto worldData = world->worldDataForProcess(process);
                    process->sendWithAsyncReply(Messages::WebPage::ExecuteExtensionTabScript(this->identifier(), worldData, *ready, *url, parameters.matchAboutBlank, source, sourceURL, userGesture),
                        [results, index, aggregator](WebExtensionTabScriptExecutionResult&& result) mutable {
                            (*results)[index] = WTF::move(result);
                        }, page->webPageIDInProcess(process));
                }, page->webPageIDInProcess(process));
        }
    });
}

}
#endif
