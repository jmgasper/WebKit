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

#include "APIContentWorld.h"
#include "APIUserStyleSheet.h"
#include "FrameTreeNodeData.h"
#include "MessageSenderInlines.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionMessageTargetParameters.h"
#include "WebExtensionPermission.h"
#include "WebExtensionRegisteredScriptParameters.h"
#include "WebExtensionScriptInjectionParameters.h"
#include "WebExtensionTabScriptExecution.h"
#include "WebExtensionUtilities.h"
#include "WebPageMessages.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebUserContentControllerProxy.h"
#include <WebCore/UserStyleSheet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/MonotonicTime.h>
#include <wtf/Box.h>
#include <wtf/CallbackAggregator.h>

// Ported from Cocoa/API/WebExtensionContextAPIScriptingCocoa.mm, the CSS and
// port parts of WebExtensionContextAPITabsCocoa.mm, and the style sheet helpers
// of Cocoa/WebExtensionDynamicScriptsCocoa.mm. Script execution reuses the
// native tab-script path of WebExtensionContextAPITabScriptHaiku.cpp.

namespace WebKit {

using namespace WebExtensionDynamicScripts;

static std::optional<SourcePair> nativeSourcePairForResource(const String& path, WebExtensionContext& context)
{
    URL resourceURL { context.baseURL(), path };
    if (path.isEmpty() || path.contains('\0') || !context.isURLForThisExtension(resourceURL))
        return std::nullopt;
    Ref extension = context.extension();
    auto resource = extension->resourceStringForPath(path, WebExtension::CacheResult::Yes);
    if (!resource) {
        context.recordErrorIfNeeded(resource.error());
        return std::nullopt;
    }
    auto source = context.localizedResourceString(*resource, extension->resourceMIMETypeForPath(path));
    return SourcePair { WTF::move(source), WTF::move(resourceURL) };
}

// Null when a packaged file cannot be read: a partial injection would leave
// the page in a state the extension never asked for.
static std::optional<SourcePairs> nativeSourcePairs(const WebExtensionScriptInjectionParameters& parameters, WebExtensionContext& context)
{
    SourcePairs result;
    if (parameters.files) {
        for (auto& file : *parameters.files) {
            auto pair = nativeSourcePairForResource(file, context);
            if (!pair)
                return std::nullopt;
            result.append(WTF::move(*pair));
        }
        return result;
    }
    result.append(SourcePair { parameters.code.value_or(parameters.css.value_or(emptyString())), URL { } });
    return result;
}

// Host or activeTab access to the committed document, never the `tabs`
// metadata permission. File documents additionally need file access.
static bool canInjectIntoNativeTab(WebExtensionContext& context, WebExtensionTab& tab)
{
    auto url = tab.url();
    if (!url.isValid() || !(url.protocolIsInHTTPFamily() || url.protocolIsFile()))
        return false;
    if (url.protocolIsFile() && !context.hasAccessToFileURLs())
        return false;
    return tab.extensionHasAccess() && context.hasPermission(url, &tab, { });
}

// Upstream attaches page style sheets without URL limits, so they outlive the
// document that was authorized. Confine them to what this extension may access
// now; a later navigation elsewhere simply stops matching.
static Vector<String> nativeStyleSheetAllowlist(WebExtensionContext& context, WebExtensionTab& tab)
{
    OptionSet<WebExtensionMatchPattern::Options> options;
    if (context.hasAccessToFileURLs())
        options.add(WebExtensionMatchPattern::Options::AllowFileScheme);
    Vector<String> result;
    for (Ref pattern : context.currentPermissionMatchPatterns())
        result.appendVector(pattern->expandedStrings(options));
    if (RefPtr pattern = tab.temporaryPermissionMatchPattern())
        result.appendVector(pattern->expandedStrings(options));
    return result;
}

// SUMMIT_EXTENSION_CSS_TRACE=1: each style sheet an extension adds to or
// removes from a page (tabs.insertCSS(), scripting.insertCSS()). Every one
// makes the page work out the style of everything in it again, its shadow
// trees included.
static bool extensionCSSTraceHaiku()
{
    static const bool enabled = [] {
        const char* value = getenv("SUMMIT_EXTENSION_CSS_TRACE");
        return value && !strcmp(value, "1");
    }();
    return enabled;
}

static void injectNativeStyleSheets(const SourcePairs& styleSheets, WebPageProxy& page, API::ContentWorld& world, WebCore::UserStyleLevel level, WebCore::UserContentInjectedFrames frames, Vector<String>&& allowlist, WebExtensionContext& context)
{
    Ref controller = page.userContentController();
    if (extensionCSSTraceHaiku()) {
        size_t bytes = 0;
        for (auto& styleSheet : styleSheets)
            bytes += styleSheet.first.length();
        fprintf(stderr, "Summit extension css: %.3f insert %zu sheets, %zu chars, level %d, %s frames, page %llu, %zu injected\n", MonotonicTime::now().secondsSinceEpoch().seconds(),
            styleSheets.size(), bytes, static_cast<int>(level), frames == WebCore::UserContentInjectedFrames::InjectInAllFrames ? "all" : "top", static_cast<unsigned long long>(page.identifier().toUInt64()), context.dynamicallyInjectedUserStyleSheets().size());
    }
    for (auto& styleSheet : styleSheets) {
        Ref userStyleSheet = API::UserStyleSheet::create(WebCore::UserStyleSheet { styleSheet.first, styleSheet.second, Vector<String> { allowlist }, { }, frames, WebCore::UserContentMatchParentFrame::Never, level, std::nullopt }, world, &page);
        controller->addUserStyleSheet(userStyleSheet);
        context.dynamicallyInjectedUserStyleSheets().append(WTF::move(userStyleSheet));
    }
}

static void removeNativeStyleSheets(const SourcePairs& styleSheets, WebPageProxy& page, WebCore::UserContentInjectedFrames frames, WebExtensionContext& context)
{
    Ref controller = page.userContentController();
    if (extensionCSSTraceHaiku())
        fprintf(stderr, "Summit extension css: %.3f remove %zu sheets, page %llu\n", MonotonicTime::now().secondsSinceEpoch().seconds(), styleSheets.size(), static_cast<unsigned long long>(page.identifier().toUInt64()));
    auto& injected = context.dynamicallyInjectedUserStyleSheets();
    for (auto& styleSheet : styleSheets) {
        UserStyleSheetVector matches;
        for (auto& candidate : injected) {
            auto& sheet = candidate->userStyleSheet();
            if (sheet.source() == styleSheet.first && sheet.injectedFrames() == frames)
                matches.append(candidate);
        }
        for (auto& match : matches) {
            controller->removeUserStyleSheet(match);
            injected.removeFirst(match);
        }
    }
}

static WebCore::UserContentInjectedFrames nativeInjectedFrames(const WebExtensionScriptInjectionParameters& parameters)
{
    // FIXME: <https://webkit.org/b/262491> There is currently no way to inject CSS in specific frames based on ID's. If 'frameIds' is passed, default to the main frame.
    return parameters.frameIdentifiers || parameters.documentIdentifiers ? WebCore::UserContentInjectedFrames::InjectInTopFrameOnly : WebCore::UserContentInjectedFrames::InjectInAllFrames;
}

void WebExtensionContext::insertNativeCSS(WebExtensionTab* tab, const WebExtensionScriptInjectionParameters& parameters, const String& apiName, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr page = tab ? tab->nativePage() : nullptr;
    if (!tab || !tab->isOpen() || !page) {
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    if (!safeToInjectContent() || !m_contentScriptWorld) {
        completionHandler(toWebExtensionError(apiName, nullString(), "could not inject stylesheet on this tab"_s));
        return;
    }

    if (!canInjectIntoNativeTab(*this, *tab)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "this extension does not have access to this tab"_s));
        return;
    }

    auto styleSheets = nativeSourcePairs(parameters, *this);
    if (!styleSheets) {
        completionHandler(toWebExtensionError(apiName, "files"_s, "a style sheet is not a readable resource of this extension"_s));
        return;
    }

    injectNativeStyleSheets(*styleSheets, *page, Ref { *m_contentScriptWorld }, parameters.styleLevel, nativeInjectedFrames(parameters), nativeStyleSheetAllowlist(*this, *tab), *this);
    completionHandler({ });
}

void WebExtensionContext::removeNativeCSS(WebExtensionTab* tab, const WebExtensionScriptInjectionParameters& parameters, const String& apiName, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr page = tab ? tab->nativePage() : nullptr;
    if (!tab || !tab->isOpen() || !page) {
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    // Allow removing CSS without permission, since it is not sensitive and the extension might have had permission before
    // and permission has been revoked since it inserted CSS. This allows for the extension to clean up.
    auto styleSheets = nativeSourcePairs(parameters, *this);
    if (!styleSheets) {
        completionHandler(toWebExtensionError(apiName, "files"_s, "a style sheet is not a readable resource of this extension"_s));
        return;
    }

    removeNativeStyleSheets(*styleSheets, *page, nativeInjectedFrames(parameters), *this);
    completionHandler({ });
}

void WebExtensionContext::tabsInsertCSS(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.insertCSS()"_s;
    if (extension().supportsManifestVersion(3)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "it is only available to Manifest V2 extensions"_s));
        return;
    }

    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    insertNativeCSS(tab.get(), parameters, apiName, WTF::move(completionHandler));
}

void WebExtensionContext::tabsRemoveCSS(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.removeCSS()"_s;
    if (extension().supportsManifestVersion(3)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "it is only available to Manifest V2 extensions"_s));
        return;
    }

    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    removeNativeCSS(tab.get(), parameters, apiName, WTF::move(completionHandler));
}

void WebExtensionContext::tabsConnect(WebExtensionTabIdentifier tabIdentifier, WebExtensionPortChannelIdentifier channelIdentifier, String name, const WebExtensionMessageTargetParameters& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.connect()"_s;

    constexpr auto sourceContentWorldType = WebExtensionContentWorldType::Main;

    RefPtr tab = getTab(tabIdentifier);
    RefPtr page = tab && tab->isOpen() ? tab->nativePage() : nullptr;
    auto targetContentWorldType = tab && isURLForThisExtension(tab->url()) ? WebExtensionContentWorldType::Main : WebExtensionContentWorldType::ContentScript;

    // Add 1 for the starting port here so disconnect will balance with a decrement.
    addPorts(sourceContentWorldType, targetContentWorldType, channelIdentifier, { senderParameters.pageProxyIdentifier });

    if (!page) {
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    constexpr auto eventType = WebExtensionEventListenerType::RuntimeOnConnect;
    auto targets = processes({ eventType }, { targetContentWorldType }, [&](auto&, auto& listenerPage, auto&) {
        return listenerPage.identifier() == page->identifier();
    });
    if (targets.isEmpty()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "no runtime.onConnect listeners found"_s));
        return;
    }

    // The target remains in this extension's namespace and is checked again
    // against the actual frame and document in each receiving web process.
    auto scopedTarget = targetParameters;
    scopedTarget.pageProxyIdentifier = page->identifier();

    auto handledCount = Box<size_t>::create(0);
    size_t totalExpected = targets.size();

    for (Ref process : targets) {
        process->sendWithAsyncReply(Messages::WebExtensionContextProxy::DispatchRuntimeConnectEvent(targetContentWorldType, channelIdentifier, name, scopedTarget, senderParameters, userGesture), [=, this, protectedThis = Ref { *this }](HashCountedSet<WebPageProxyIdentifier>&& addedPortCounts) mutable {
            // Flip target and source worlds since we're adding the opposite side of the port connection, sending from target back to source.
            addPorts(targetContentWorldType, sourceContentWorldType, channelIdentifier, WTF::move(addedPortCounts));

            fireQueuedPortMessageEventsIfNeeded(targetContentWorldType, channelIdentifier);
            fireQueuedPortMessageEventsIfNeeded(sourceContentWorldType, channelIdentifier);

            firePortDisconnectEventIfNeeded(sourceContentWorldType, targetContentWorldType, channelIdentifier);

            if (++*handledCount < totalExpected)
                return;

            clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
            clearQueuedPortMessages(sourceContentWorldType, channelIdentifier);
        }, identifier());
    }

    completionHandler({ });
}

bool WebExtensionContext::isScriptingMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasPermission(WebExtensionPermission::scripting());
}

void WebExtensionContext::scriptingInsertCSS(const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr tab = parameters.tabIdentifier ? getTab(*parameters.tabIdentifier) : nullptr;
    insertNativeCSS(tab.get(), parameters, "scripting.insertCSS()"_s, WTF::move(completionHandler));
}

void WebExtensionContext::scriptingRemoveCSS(const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    if (m_dynamicallyInjectedUserStyleSheets.isEmpty()) {
        completionHandler({ });
        return;
    }

    RefPtr tab = parameters.tabIdentifier ? getTab(*parameters.tabIdentifier) : nullptr;
    removeNativeCSS(tab.get(), parameters, "scripting.removeCSS()"_s, WTF::move(completionHandler));
}

void WebExtensionContext::scriptingExecuteScriptNative(const WebExtensionScriptInjectionParameters& parameters, bool injectImmediately, bool userGesture, CompletionHandler<void(std::expected<Vector<WebExtensionTabScriptFrameResult>, WebExtensionError>&&)>&& completion)
{
    static constexpr auto apiName = "scripting.executeScript()"_s;

    RefPtr tab = parameters.tabIdentifier ? getTab(*parameters.tabIdentifier) : nullptr;
    RefPtr page = tab && tab->isOpen() ? tab->nativePage() : nullptr;
    if (!page) {
        completion(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    if (!safeToInjectContent() || !m_contentScriptWorld) {
        completion(toWebExtensionError(apiName, nullString(), "could not execute script on this tab"_s));
        return;
    }

    // world: 'MAIN' evaluates in the page's own world (AdGuard and Privacy Badger
    // scriptlets, Proton Pass and LastPass form helpers, Tampermonkey). Host
    // access is checked exactly as for the isolated world.
    if (parameters.world != WebExtensionContentWorldType::ContentScript && parameters.world != WebExtensionContentWorldType::Main) {
        completion(toWebExtensionError(apiName, "world"_s, "it must be 'ISOLATED' or 'MAIN'"_s));
        return;
    }
    bool mainWorld = parameters.world == WebExtensionContentWorldType::Main;

    if (!canInjectIntoNativeTab(*this, *tab)) {
        completion(toWebExtensionError(apiName, nullString(), "this extension does not have access to this tab"_s));
        return;
    }

    if (parameters.files.has_value() == parameters.code.has_value()) {
        completion(toWebExtensionError(apiName, nullString(), "exactly one of 'files' or 'func' is required"_s));
        return;
    }

    auto sources = nativeSourcePairs(parameters, *this);
    if (!sources || sources->isEmpty()) {
        completion(toWebExtensionError(apiName, "files"_s, "a script is not a readable resource of this extension"_s));
        return;
    }
    for (auto& source : *sources) {
        if (source.first.length() > webExtensionMaxMessageLength) {
            completion(toWebExtensionError(apiName, nullString(), "the script exceeds the maximum size"_s));
            return;
        }
    }

    Ref world = *m_contentScriptWorld;
    auto stillCurrent = [protectedThis = Ref { *this }, tab, page, world] {
        return protectedThis->safeToInjectContent() && protectedThis->m_contentScriptWorld == world.ptr()
            && tab->isOpen() && tab->extensionHasAccess() && tab->nativePage() == page;
    };
    auto runAt = injectImmediately ? WebExtensionTabScriptRunAt::DocumentStart : WebExtensionTabScriptRunAt::DocumentIdle;

    page->getAllFrames([this, protectedThis = Ref { *this }, tab, page, world, mainWorld, parameters, sources = WTF::move(*sources), runAt, userGesture, stillCurrent, completion = WTF::move(completion)](std::optional<FrameTreeNodeData>&& frames) mutable {
        if (!frames || !stillCurrent()) {
            completion(toWebExtensionError(apiName, nullString(), "the target document is unavailable"_s));
            return;
        }

        // Each requested frame or document is captured on its own, then merged
        // in request order without visiting the same frame twice.
        Vector<WebExtensionTabScriptTarget> targets;
        auto capture = [&](const WebExtensionTabScriptParameters& selection) -> std::optional<String> {
            auto captured = captureWebExtensionTabScriptTargets(*frames, page->identifier(), selection);
            if (!captured)
                return captured.error();
            for (auto& target : *captured) {
                bool known = targets.containsIf([&](auto& existing) {
                    return existing.documents.last().frameIdentifier == target.documents.last().frameIdentifier;
                });
                if (!known)
                    targets.append(WTF::move(target));
            }
            return std::nullopt;
        };
        std::optional<String> captureError;
        if (parameters.documentIdentifiers) {
            for (auto& documentIdentifier : *parameters.documentIdentifiers) {
                WebExtensionTabScriptParameters selection;
                selection.documentIdentifier = documentIdentifier;
                if ((captureError = capture(selection)))
                    break;
            }
        } else if (parameters.frameIdentifiers) {
            for (auto& frameIdentifier : *parameters.frameIdentifiers) {
                WebExtensionTabScriptParameters selection;
                selection.frameIdentifier = frameIdentifier;
                if ((captureError = capture(selection)))
                    break;
            }
        } else {
            WebExtensionTabScriptParameters selection;
            selection.allFrames = true;
            captureError = capture(selection);
        }
        if (captureError || targets.isEmpty()) {
            completion(toWebExtensionError(apiName, nullString(), captureError.value_or("the requested frame or document was not found"_s)));
            return;
        }

        auto frameResult = [](const WebExtensionTabScriptTarget& target) {
            WebExtensionTabScriptFrameResult result;
            auto& document = target.documents.last();
            result.frameIdentifier = target.documents.size() == 1 ? WebExtensionFrameConstants::MainFrameIdentifier : toWebExtensionFrameIdentifier(std::optional { document.frameIdentifier });
            result.documentIdentifier = document.documentIdentifier.object();
            return result;
        };

        auto results = Box<Vector<WebExtensionTabScriptFrameResult>>::create();
        for (auto& target : targets)
            results->append(frameResult(target));
        Ref aggregator = MainRunLoopCallbackAggregator::create([results, completion = WTF::move(completion)]() mutable {
            completion(WTF::move(*results));
        });

        for (size_t index = 0; index < targets.size(); ++index) {
            auto target = targets[index];
            auto url = webExtensionTabScriptPermissionURL(target, false);
            // Use host or activeTab grants already approved by the native browser.
            // The tabs metadata permission must not authorize script execution.
            if (!url || (url->protocolIs("file"_s) && !hasAccessToFileURLs()) || !hasPermission(*url, tab.get(), { })) {
                (*results)[index].error = "this extension does not have access to this frame"_s;
                continue;
            }

            auto frameIdentifier = target.documents.last().frameIdentifier;
            Ref process = page->processContainingFrame(frameIdentifier);
            process->sendWithAsyncReply(Messages::WebPage::WaitForExtensionTabScript(this->identifier(), world->identifier(), target, runAt),
                [this, protectedThis, tab, page, world, mainWorld, sources, userGesture, stillCurrent, frameIdentifier, results, index, aggregator](std::expected<WebExtensionTabScriptTarget, String>&& ready) mutable {
                    if (!ready || !stillCurrent()) {
                        (*results)[index].error = ready ? "the extension or tab changed while waiting to inject"_s : ready.error();
                        return;
                    }
                    // Recheck after document readiness; a grant may expire
                    // or be revoked while the request waits for resources.
                    auto url = webExtensionTabScriptPermissionURL(*ready, false);
                    if (!url || (url->protocolIs("file"_s) && !hasAccessToFileURLs()) || !hasPermission(*url, tab.get(), { })) {
                        (*results)[index].error = "host access was lost before script execution"_s;
                        return;
                    }
                    executeNativeScriptSources(*page, world, mainWorld, *ready, *url, WTF::move(sources), 0, userGesture, [results, index, aggregator](WebExtensionTabScriptExecutionResult&& result) {
                        if (result)
                            (*results)[index].value = WTF::move(*result);
                        else
                            (*results)[index].error = result.error();
                    });
                }, page->webPageIDInProcess(process));
        }
    });
}

void WebExtensionContext::executeNativeScriptSources(WebPageProxy& page, API::ContentWorld& world, bool mainWorld, const WebExtensionTabScriptTarget& target, const URL& authorizedURL, SourcePairs&& sources, size_t index, bool userGesture, CompletionHandler<void(WebExtensionTabScriptExecutionResult&&)>&& completion)
{
    // `world` is always this extension's content-script world: it identifies the
    // load the request belongs to. mainWorld selects where the source runs.
    // Files run in order in the same frame; the last one supplies the result,
    // and the first failure ends the sequence.
    ASSERT(index < sources.size());
    auto frameIdentifier = target.documents.last().frameIdentifier;
    Ref process = page.processContainingFrame(frameIdentifier);
    auto worldData = (mainWorld ? API::ContentWorld::pageContentWorldSingleton() : world).worldDataForProcess(process);
    auto source = sources[index].first;
    auto sourceURL = sources[index].second;
    process->sendWithAsyncReply(Messages::WebPage::ExecuteExtensionTabScript(identifier(), worldData, target, authorizedURL, false, source, sourceURL, userGesture),
        [this, protectedThis = Ref { *this }, page = Ref { page }, world = Ref { world }, mainWorld, target, authorizedURL, sources = WTF::move(sources), index, userGesture, completion = WTF::move(completion)](WebExtensionTabScriptExecutionResult&& result) mutable {
            if (!result || index + 1 == sources.size() || !safeToInjectContent() || m_contentScriptWorld != world.ptr() || page->isClosed()) {
                completion(WTF::move(result));
                return;
            }
            executeNativeScriptSources(page, world, mainWorld, target, authorizedURL, WTF::move(sources), index + 1, userGesture, WTF::move(completion));
        }, page.webPageIDInProcess(process));
}

static Ref<JSON::Array> toJSONSerialization(const Vector<String>& items)
{
    Ref result = JSON::Array::create();
    for (const auto& item : items)
        result->pushString(item);
    return result;
}

static Vector<Ref<JSON::Object>> toJSONSerialization(const Vector<WebExtensionRegisteredScriptParameters>& scripts)
{
    Vector<Ref<JSON::Object>> result;

    for (const auto& parameters : scripts) {
        Ref object = JSON::Object::create();

        ASSERT(parameters.matchPatterns);
        ASSERT(parameters.persistent);

        object->setString("id"_s, parameters.identifier);
        object->setArray("matches"_s, toJSONSerialization(parameters.matchPatterns.value_or(Vector<String> { })));
        object->setBoolean("persistAcrossSessions"_s, parameters.persistent.value_or(true));

        if (parameters.css)
            object->setArray("css"_s, toJSONSerialization(parameters.css.value()));

        if (parameters.js)
            object->setArray("js"_s, toJSONSerialization(parameters.js.value()));

        if (parameters.excludeMatchPatterns)
            object->setArray("excludeMatches"_s, toJSONSerialization(parameters.excludeMatchPatterns.value()));

        if (parameters.allFrames)
            object->setBoolean("allFrames"_s, parameters.allFrames.value());

        if (parameters.injectionTime) {
            switch (parameters.injectionTime.value()) {
            case WebExtension::InjectionTime::DocumentStart:
                object->setString("runAt"_s, "document_start"_s);
                break;
            case WebExtension::InjectionTime::DocumentEnd:
                object->setString("runAt"_s, "document_end"_s);
                break;
            case WebExtension::InjectionTime::DocumentIdle:
                object->setString("runAt"_s, "document_idle"_s);
                break;
            }
        }

        if (parameters.styleLevel)
            object->setString("cssOrigin"_s, parameters.styleLevel.value() == WebCore::UserStyleLevel::User ? "user"_s : "author"_s);

        if (parameters.world)
            object->setString("world"_s, parameters.world.value() == WebExtensionContentWorldType::Main ? "main"_s : "isolated"_s);

        result.append(WTF::move(object));
    }

    return result;
}

void WebExtensionContext::scriptingRegisterContentScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "scripting.registerContentScripts()"_s;

    DynamicInjectedContentsMap injectedContentsMap;
    String errorMessage;
    if (!createInjectedContentForScripts(scripts, WebExtensionRegisteredScript::FirstTimeRegistration::Yes, injectedContentsMap, apiName, &errorMessage)) {
        completionHandler(makeUnexpected(errorMessage));
        return;
    }

    auto generation = m_registeredContentScriptsGeneration;
    registeredContentScriptsStore()->addScripts(toJSONSerialization(scripts), [this, protectedThis = Ref { *this }, generation, scripts, injectedContentsMap = WTF::move(injectedContentsMap), completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
            return;
        }

        // The store outlives an unload; never revive scripts of an older load.
        if (!isLoaded() || generation != m_registeredContentScriptsGeneration) {
            completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
            return;
        }

        InjectedContentVector injectedContents;
        for (auto& parameters : scripts) {
            auto injectedContent = injectedContentsMap.get(parameters.identifier);
            m_registeredScriptsMap.set(parameters.identifier, WebExtensionRegisteredScript::create(*this, parameters, injectedContent));
            injectedContents.append(WTF::move(injectedContent));
        }

        addInjectedContent(injectedContents);

        completionHandler({ });
    });
}

void WebExtensionContext::scriptingUpdateRegisteredScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "scripting.updateContentScripts()"_s;

    Vector<WebExtensionRegisteredScriptParameters> updatedParameters;

    for (auto parameters : scripts) {
        auto scriptID = parameters.identifier;
        RefPtr registeredScript = m_registeredScriptsMap.get(scriptID);
        if (!registeredScript) {
            completionHandler(toWebExtensionError(apiName, nullString(), makeString("no existing script with ID '"_s, scriptID, "'"_s)));
            return;
        }

        registeredScript->merge(parameters);
        updatedParameters.append(parameters);
    }

    String errorMessage;
    DynamicInjectedContentsMap injectedContentsMap;
    if (!createInjectedContentForScripts(updatedParameters, WebExtensionRegisteredScript::FirstTimeRegistration::No, injectedContentsMap, apiName, &errorMessage)) {
        completionHandler(makeUnexpected(errorMessage));
        return;
    }

    auto generation = m_registeredContentScriptsGeneration;
    auto updatedScripts = toJSONSerialization(updatedParameters);
    registeredContentScriptsStore()->updateScripts(WTF::move(updatedScripts), [this, protectedThis = Ref { *this }, generation, scripts = WTF::move(updatedParameters), injectedContentsMap = WTF::move(injectedContentsMap), completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
            return;
        }

        if (!isLoaded() || generation != m_registeredContentScriptsGeneration) {
            completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
            return;
        }

        InjectedContentVector injectedContents;
        for (auto& parameters : scripts) {
            auto scriptID = parameters.identifier;
            RefPtr registeredScript = m_registeredScriptsMap.get(scriptID);
            if (!registeredScript)
                continue;

            registeredScript->updateParameters(parameters);
            registeredScript->removeUserScriptsAndStyleSheets(scriptID);

            auto injectedContent = injectedContentsMap.get(scriptID);
            registeredScript->updateInjectedContent(injectedContent);
            injectedContents.append(WTF::move(injectedContent));
        }

        addInjectedContent(injectedContents);

        completionHandler({ });
    });
}

void WebExtensionContext::scriptingGetRegisteredScripts(const Vector<String>& scriptIDs, CompletionHandler<void(std::expected<Vector<WebExtensionRegisteredScriptParameters>, WebExtensionError>&&)>&& completionHandler)
{
    Vector<WebExtensionRegisteredScriptParameters> scripts;

    if (scriptIDs.isEmpty()) {
        // Return all registered scripts if no filter is specififed.
        for (auto& entry : m_registeredScriptsMap)
            scripts.append(protect(entry.value.get())->parameters());
    } else {
        for (auto& scriptID : scriptIDs) {
            RefPtr registeredScript = m_registeredScriptsMap.get(scriptID);
            if (!registeredScript)
                continue;

            scripts.append(registeredScript->parameters());
        }
    }

    completionHandler(WTF::move(scripts));
}

void WebExtensionContext::scriptingUnregisterContentScripts(const Vector<String>& scriptIDs, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "scripting.unregisterContentScripts()"_s;

    // Remove all registered scripts if no filter is specififed.
    auto ids = scriptIDs.isEmpty() ? copyToVector(m_registeredScriptsMap.keys()) : scriptIDs;

    for (auto& scriptID : ids) {
        if (!m_registeredScriptsMap.contains(scriptID)) {
            completionHandler(toWebExtensionError(apiName, nullString(), makeString("no script with ID '"_s, scriptID, "'"_s)));
            return;
        }
    }

    auto generation = m_registeredContentScriptsGeneration;
    registeredContentScriptsStore()->deleteScriptsWithIDs(ids, [this, protectedThis = Ref { *this }, generation, ids, completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
            return;
        }

        if (!isLoaded() || generation != m_registeredContentScriptsGeneration) {
            completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
            return;
        }

        for (auto& scriptID : ids) {
            if (RefPtr registeredScript = m_registeredScriptsMap.take(scriptID))
                registeredScript->removeUserScriptsAndStyleSheets(scriptID);
        }

        completionHandler({ });
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
