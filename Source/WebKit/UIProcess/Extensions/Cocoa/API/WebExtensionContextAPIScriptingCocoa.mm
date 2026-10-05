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

#if !__has_feature(objc_arc)
#error This file requires ARC. Add the "-fobjc-arc" compiler flag for this file.
#endif

#import "config.h"
#import "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#import "APIError.h"
#import "CocoaHelpers.h"
#import "WKFrameInfoPrivate.h"
#import "WKNSError.h"
#import "WKWebViewInternal.h"
#import "WKWebViewPrivate.h"
#import "WebExtensionAPIScripting.h"
#import "WebExtensionContextProxy.h"
#import "WebExtensionContextProxyMessages.h"
#import "WebExtensionPermission.h"
#import "WebExtensionRegisteredScriptParameters.h"
#import "WebExtensionScriptInjectionParameters.h"
#import "WebExtensionTab.h"
#import "WebExtensionTabIdentifier.h"
#import "WebExtensionUtilities.h"
#import "_WKFrameHandle.h"
#import "_WKFrameTreeNode.h"
#import <WebCore/UserStyleSheet.h>
#import <wtf/BlockPtr.h>
#import <wtf/cocoa/VectorCocoa.h>

namespace WebKit {

using namespace WebExtensionDynamicScripts;

static NSString * const idKey = @"id";
static NSString * const matchesKey = @"matches";
static NSString * const excludeMatchesKey = @"excludeMatches";
static NSString * const persistAcrossSessionsKey = @"persistAcrossSessions";
static NSString * const jsKey = @"js";
static NSString * const cssKey = @"css";
static NSString * const cssOriginKey = @"cssOrigin";
static NSString * const allFramesKey = @"allFrames";
static NSString * const runAtKey = @"runAt";
static NSString * const userValue = @"user";
static NSString * const authorValue = @"author";

static NSString * const worldKey = @"world";
static NSString * const mainWorld = @"main";
static NSString * const isolatedWorld = @"isolated";

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

        object->setString(idKey, parameters.identifier);
        object->setArray(matchesKey, toJSONSerialization(parameters.matchPatterns.value()));
        object->setBoolean(persistAcrossSessionsKey, parameters.persistent.value());

        if (parameters.css)
            object->setArray(cssKey, toJSONSerialization(parameters.css.value()));

        if (parameters.js)
            object->setArray(jsKey, toJSONSerialization(parameters.js.value()));

        if (parameters.excludeMatchPatterns)
            object->setArray(excludeMatchesKey, toJSONSerialization(parameters.excludeMatchPatterns.value()));

        if (parameters.allFrames)
            object->setBoolean(allFramesKey, parameters.allFrames.value());

        if (parameters.injectionTime)
            object->setString(runAtKey, toWebAPI(parameters.injectionTime.value()));

        if (parameters.styleLevel)
            object->setString(cssOriginKey, parameters.styleLevel.value() == WebCore::UserStyleLevel::User ? userValue : authorValue);

        if (parameters.world)
            object->setString(worldKey, parameters.world.value() == WebExtensionContentWorldType::Main ? mainWorld : isolatedWorld);

        result.append(object);
    }

    return result;
}

bool WebExtensionContext::isScriptingMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasPermission(WebExtensionPermission::scripting());
}

void WebExtensionContext::scriptingExecuteScript(const WebExtensionScriptInjectionParameters& parameters, bool userGesture, CompletionHandler<void(std::expected<InjectionResults, WebExtensionError>&&)>&& completionHandler)
{
    static NSString * const apiName= @"scripting.executeScript()";

    RefPtr tab = getTab(parameters.tabIdentifier.value());
    if (!tab) {
        completionHandler(toWebExtensionError(apiName, nullString(), @"tab not found"));
        return;
    }

    requestPermissionToAccessURLs({ tab->url() }, tab, [this, protectedThis = Ref { *this }, tab, parameters, userGesture, completionHandler = WTF::move(completionHandler)](auto&& requestedURLs, auto&& allowedURLs, auto expirationDate) mutable {
        if (!tab->extensionHasPermission()) {
            completionHandler(toWebExtensionError(apiName, nullString(), @"this extension does not have access to this tab"));
            return;
        }

        auto *webView = tab->webView();
        if (!webView) {
            completionHandler(toWebExtensionError(apiName, nullString(), @"could not execute script on this tab"));
            return;
        }

        auto scriptPairs = getSourcePairsForParameters(parameters, *this);
        Ref executionWorld = toContentWorld(parameters.world);

        executeScript(scriptPairs, webView, executionWorld, *tab, parameters, *this, userGesture, [completionHandler = WTF::move(completionHandler)](InjectionResults&& injectionResults) mutable {
            completionHandler(WTF::move(injectionResults));
        });
    });
}

void WebExtensionContext::scriptingInsertCSS(const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static NSString * const apiName= @"scripting.insertCSS()";

    RefPtr tab = getTab(parameters.tabIdentifier.value());
    if (!tab) {
        completionHandler(toWebExtensionError(apiName, nullString(), @"tab not found"));
        return;
    }

    requestPermissionToAccessURLs({ tab->url() }, tab, [this, protectedThis = Ref { *this }, tab, parameters, completionHandler = WTF::move(completionHandler)](auto&& requestedURLs, auto&& allowedURLs, auto expirationDate) mutable {
        if (!tab->extensionHasPermission()) {
            completionHandler(toWebExtensionError(apiName, nullString(), @"this extension does not have access to this tab"));
            return;
        }

        auto *webView = tab->webView();
        if (!webView) {
            completionHandler(toWebExtensionError(apiName, nullString(), @"could not inject stylesheet on this tab"));
            return;
        }

        // FIXME: <https://webkit.org/b/262491> There is currently no way to inject CSS in specific frames based on ID's. If 'frameIds' is passed, default to the main frame.
        auto injectedFrames = parameters.frameIdentifiers ? WebCore::UserContentInjectedFrames::InjectInTopFrameOnly : WebCore::UserContentInjectedFrames::InjectInAllFrames;

        auto styleSheetPairs = getSourcePairsForParameters(parameters, *this);
        injectStyleSheets(styleSheetPairs, webView, Ref { *m_contentScriptWorld }, parameters.styleLevel, injectedFrames, *this);

        completionHandler({ });
    });
}

void WebExtensionContext::scriptingRemoveCSS(const WebExtensionScriptInjectionParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    if (m_dynamicallyInjectedUserStyleSheets.isEmpty()) {
        completionHandler({ });
        return;
    }

    static NSString * const apiName= @"scripting.removeCSS()";

    RefPtr tab = getTab(parameters.tabIdentifier.value());
    if (!tab) {
        completionHandler(toWebExtensionError(apiName, nullString(), @"tab not found"));
        return;
    }

    auto *webView = tab->webView();
    if (!webView) {
        completionHandler(toWebExtensionError(apiName, nullString(), @"could not remove stylesheet from this tab"));
        return;
    }

    // Allow removing CSS without permission, since it is not sensitive and the extension might have had permission before
    // and permission has been revoked since it inserted CSS. This allows for the extension to clean up.

    // FIXME: <https://webkit.org/b/262491> There is currently no way to inject CSS in specific frames based on ID's. If 'frameIds' is passed, default to the main frame.
    auto injectedFrames = parameters.frameIdentifiers ? WebCore::UserContentInjectedFrames::InjectInTopFrameOnly : WebCore::UserContentInjectedFrames::InjectInAllFrames;

    auto styleSheetPairs = getSourcePairsForParameters(parameters, *this);
    removeStyleSheets(styleSheetPairs, webView, injectedFrames, *this);

    completionHandler({ });
}

void WebExtensionContext::scriptingRegisterContentScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static NSString * const apiName= @"scripting.registerContentScripts()";

    DynamicInjectedContentsMap injectedContentsMap;
    NSString *errorMessage;
    if (!createInjectedContentForScripts(scripts, FirstTimeRegistration::Yes, injectedContentsMap, apiName, &errorMessage)) {
        completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
        return;
    }

    auto serializedScripts = toJSONSerialization(scripts);
    registeredContentScriptsStore()->addScripts(serializedScripts, [this, protectedThis = Ref { *this }, scripts, injectedContentsMap = WTF::move(injectedContentsMap), completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
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
    static NSString * const apiName= @"scripting.updateContentScripts()";

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

    NSString *errorMessage;
    DynamicInjectedContentsMap injectedContentsMap;
    if (!createInjectedContentForScripts(updatedParameters, FirstTimeRegistration::No, injectedContentsMap, apiName, &errorMessage)) {
        completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
        return;
    }

    auto updatedScripts = toJSONSerialization(updatedParameters);
    registeredContentScriptsStore()->updateScripts(updatedScripts, [this, protectedThis = Ref { *this }, scripts = WTF::move(updatedParameters), injectedContentsMap = WTF::move(injectedContentsMap), completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
            return;
        }

        InjectedContentVector injectedContents;
        for (auto& parameters : scripts) {
            auto scriptID = parameters.identifier;
            RefPtr registeredScript = m_registeredScriptsMap.get(scriptID);
            ASSERT(registeredScript);

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
    static NSString * const apiName = @"scripting.unregisterContentScripts()";

    // Remove all registered scripts if no filter is specififed.
    auto ids = scriptIDs.isEmpty() ? copyToVector(m_registeredScriptsMap.keys()) : scriptIDs;

    for (auto& scriptID : ids) {
        if (!m_registeredScriptsMap.contains(scriptID)) {
            completionHandler(toWebExtensionError(apiName, nullString(), makeString("no script with ID '"_s, scriptID, "'"_s)));
            return;
        }
    }

    registeredContentScriptsStore()->deleteScriptsWithIDs(ids, [this, protectedThis = Ref { *this }, ids, completionHandler = WTF::move(completionHandler)](const String& errorMessage) mutable {
        if (!errorMessage.isEmpty()) {
            completionHandler(toWebExtensionError(apiName, nullString(), errorMessage));
            return;
        }

        for (auto& scriptID : ids) {
            if (RefPtr registeredScript = m_registeredScriptsMap.take(scriptID))
                registeredScript->removeUserScriptsAndStyleSheets(scriptID);
        }

        completionHandler({ });
    });
}

bool WebExtensionContext::createInjectedContentForScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, FirstTimeRegistration firstTimeRegistration, DynamicInjectedContentsMap& injectedContentsMap, NSString *callingAPIName, NSString **errorMessage)
{
    String error;
    bool result = createInjectedContentForScripts(scripts, firstTimeRegistration, injectedContentsMap, String(callingAPIName), &error);
    *errorMessage = result ? nil : error.createNSString().autorelease();
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)

