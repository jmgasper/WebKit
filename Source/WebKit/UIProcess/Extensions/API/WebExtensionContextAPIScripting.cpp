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

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Logging.h"
#include "WebExtensionPermission.h"
#include "WebExtensionRegisteredScriptParser.h"
#include "WebExtensionUtilities.h"

namespace WebKit {

using namespace WebExtensionDynamicScripts;
using FirstTimeRegistration = WebExtensionRegisteredScript::FirstTimeRegistration;

void WebExtensionContext::loadRegisteredContentScripts()
{
    auto generation = ++m_registeredContentScriptsGeneration;
    if (!hasPermission(WebExtensionPermission::scripting()))
        return;

    registeredContentScriptsStore()->getScripts([this, protectedThis = Ref { *this }, generation](Vector<Ref<JSON::Object>> scripts, const String& errorMessage) mutable {
        if (!isLoaded() || generation != m_registeredContentScriptsGeneration || !hasPermission(WebExtensionPermission::scripting()))
            return;
        if (!errorMessage.isEmpty()) {
            RELEASE_LOG_ERROR(Extensions, "Unable to get registered scripts: %s", errorMessage.utf8().legacyCStringPointer());
            return;
        }

        auto parsed = parseWebExtensionRegisteredScripts(scripts, WebExtensionRegisteredScriptParseMode::Registration);
        if (!parsed) {
            RELEASE_LOG_ERROR(Extensions, "Failed to parse registered scripts: %s", parsed.error().utf8().legacyCStringPointer());
            return;
        }
        auto parametersVector = WTF::move(*parsed);
        DynamicInjectedContentsMap injectedContentsMap;
        String validationError;
        if (!createInjectedContentForScripts(parametersVector, FirstTimeRegistration::Yes, injectedContentsMap, nullString(), &validationError)) {
            RELEASE_LOG_ERROR(Extensions, "Failed to create injected content: %s", validationError.utf8().legacyCStringPointer());
            return;
        }

        InjectedContentVector injectedContents;
        for (auto& parameters : parametersVector) {
            auto injectedContent = injectedContentsMap.get(parameters.identifier);
            m_registeredScriptsMap.set(parameters.identifier, WebExtensionRegisteredScript::create(*this, parameters, injectedContent));
            injectedContents.append(WTF::move(injectedContent));
        }
        addInjectedContent(injectedContents);
    });
}

void WebExtensionContext::clearRegisteredContentScripts()
{
    ++m_registeredContentScriptsGeneration;
    m_registeredScriptsMap.clear();

    registeredContentScriptsStore()->deleteDatabase([](const String& errorMessage) {
        if (!errorMessage.isEmpty())
            RELEASE_LOG_ERROR(Extensions, "Failed to delete registered content scripts database. Error: %s", errorMessage.utf8().legacyCStringPointer());
    });
}

bool WebExtensionContext::createInjectedContentForScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, FirstTimeRegistration firstTimeRegistration, DynamicInjectedContentsMap& injectedContentsMap, const String& callingAPIName, String* errorMessage)
{
    *errorMessage = { };
    Vector<String> idsToAdd;

    for (auto& parameters : scripts) {
        auto scriptID = parameters.identifier;

        if (firstTimeRegistration == FirstTimeRegistration::Yes && (m_registeredScriptsMap.contains(scriptID) || idsToAdd.contains(scriptID))) {
            *errorMessage = toErrorString(callingAPIName, nullString(), makeString("duplicate ID '"_s, scriptID, "'"_s));
            return false;
        }

        idsToAdd.append(scriptID);

        HashSet<Ref<WebExtensionMatchPattern>> includeMatchPatterns;
        HashSet<Ref<WebExtensionMatchPattern>> excludeMatchPatterns;

        // Optional. The list of JavaScript files to be injected into matching pages. These are injected in the order they appear in this array.
        auto scriptPaths = parameters.js.value_or(Vector<String> { });
        scriptPaths.removeAllMatching([](auto& path) { return path.isEmpty(); });

        RefPtr extension = m_extension;
        for (auto& scriptPath : scriptPaths) {
            if (auto scriptValue = extension->resourceStringForPath(scriptPath); !scriptValue) {
                recordErrorIfNeeded(scriptValue.error());
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("invalid resource '"_s, String(scriptPath), "'"_s));
                return false;
            }
        }

        // Optional. The list of CSS files to be injected into matching pages. These are injected in the order they appear in this array, before any DOM is constructed or displayed for the page.
        auto styleSheetPaths = parameters.css.value_or(Vector<String> { });
        styleSheetPaths.removeAllMatching([](auto& path) { return path.isEmpty(); });

        for (auto& styleSheetPath : styleSheetPaths) {
            if (auto styleSheetValue = extension->resourceStringForPath(styleSheetPath); !styleSheetValue) {
                recordErrorIfNeeded(styleSheetValue.error());
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("invalid resource '"_s, String(styleSheetPath), "'"_s));
                return false;
            }
        }

        // Required for first time registration. Specifies which pages the specified scripts and stylesheets will be injected into.
        auto matchesArray = parameters.matchPatterns.value_or(Vector<String> { });
        for (auto& matchPatternString : matchesArray) {
            if (matchPatternString.isEmpty()) {
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("script with ID '"_s, scriptID, "' contains an empty match pattern"_s));
                return false;
            }

            RefPtr matchPattern = WebExtensionMatchPattern::getOrCreate(matchPatternString);
            if (!matchPattern || !matchPattern->isSupported()) {
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("script with ID '"_s, scriptID, "' has an invalid match pattern '"_s, String(matchPatternString), "'"_s));
                return false;
            }

            includeMatchPatterns.add(matchPattern.releaseNonNull());
        }

        // Optional. Excludes pages that this content script would otherwise be injected into.
        auto excludeMatchesArray = parameters.excludeMatchPatterns.value_or(Vector<String> { });
        for (auto& matchPatternString : excludeMatchesArray) {
            if (matchPatternString.isEmpty()) {
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("script with ID '"_s, scriptID, "' contains an empty exclude match pattern"_s));
                return false;
            }

            RefPtr matchPattern = WebExtensionMatchPattern::getOrCreate(matchPatternString);
            if (!matchPattern || !matchPattern->isSupported()) {
                *errorMessage = toErrorString(callingAPIName, nullString(), makeString("script with ID '"_s, scriptID, "' has an invalid exclude match pattern '"_s, String(matchPatternString), "'"_s));
                return false;
            }

            excludeMatchPatterns.add(matchPattern.releaseNonNull());
        }

        InjectedContentData injectedContentData;
        injectedContentData.identifier = parameters.identifier;
        injectedContentData.includeMatchPatterns = WTF::move(includeMatchPatterns);
        injectedContentData.excludeMatchPatterns = WTF::move(excludeMatchPatterns);
        injectedContentData.injectionTime = parameters.injectionTime.value_or(WebExtension::InjectionTime::DocumentIdle);
        injectedContentData.injectsIntoAllFrames = parameters.allFrames.value_or(false);
        injectedContentData.matchParentFrame = parameters.matchParentFrame.value_or(WebCore::UserContentMatchParentFrame::Never);
        injectedContentData.contentWorldType = parameters.world.value_or(WebExtensionContentWorldType::ContentScript);
        injectedContentData.styleLevel = parameters.styleLevel.value_or(WebCore::UserStyleLevel::Author);
        injectedContentData.scriptPaths = WTF::move(scriptPaths);
        injectedContentData.styleSheetPaths = WTF::move(styleSheetPaths);

        injectedContentsMap.add(scriptID, WTF::move(injectedContentData));
    }

    return true;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
