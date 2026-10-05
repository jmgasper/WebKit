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

#include "config.h"
#include "WebExtensionRegisteredScriptParser.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include <wtf/text/MakeString.h>

namespace WebKit {

std::expected<Vector<WebExtensionRegisteredScriptParameters>, String> parseWebExtensionRegisteredScripts(const Vector<Ref<JSON::Object>>& scripts, WebExtensionRegisteredScriptParseMode mode)
{
    Vector<WebExtensionRegisteredScriptParameters> result;
    for (auto& script : scripts) {
        auto invalid = [](ASCIILiteral key, ASCIILiteral message) {
            return makeUnexpected(makeString("Invalid '"_s, key, "': "_s, message));
        };
        for (auto key : { "id"_s, "runAt"_s, "cssOrigin"_s, "world"_s }) {
            if (RefPtr value = script->getValue(key); value && value->type() != JSON::Value::Type::String)
                return invalid(key, "expected a string"_s);
        }
        for (auto key : { "allFrames"_s, "matchOriginAsFallback"_s, "persistAcrossSessions"_s }) {
            if (RefPtr value = script->getValue(key); value && value->type() != JSON::Value::Type::Boolean)
                return invalid(key, "expected a boolean"_s);
        }
        for (auto key : { "css"_s, "js"_s, "matches"_s, "excludeMatches"_s }) {
            RefPtr value = script->getValue(key);
            if (!value)
                continue;
            RefPtr array = value->asArray();
            if (!array)
                return invalid(key, "expected an array of strings"_s);
            for (auto& item : *array) {
                if (item->type() != JSON::Value::Type::String)
                    return invalid(key, "expected an array of strings"_s);
            }
        }

        WebExtensionRegisteredScriptParameters parameters;
        parameters.identifier = script->getString("id"_s);
        if (parameters.identifier.isEmpty())
            return invalid("id"_s, "it must not be empty"_s);
        if (parameters.identifier[0] == '_')
            return invalid("id"_s, "it must not start with '_'"_s);

        auto strings = [&](ASCIILiteral key) -> std::optional<Vector<String>> {
            RefPtr array = script->getArray(key);
            if (!array)
                return std::nullopt;
            Vector<String> items;
            for (auto& item : *array)
                items.append(item->asString());
            return items;
        };
        parameters.css = strings("css"_s);
        parameters.js = strings("js"_s);
        parameters.matchPatterns = strings("matches"_s);
        parameters.excludeMatchPatterns = strings("excludeMatches"_s);

        bool registration = mode == WebExtensionRegisteredScriptParseMode::Registration;
        if (registration && (!parameters.matchPatterns || parameters.matchPatterns->isEmpty()))
            return invalid("matches"_s, "it must specify at least one match pattern"_s);
        if (parameters.matchPatterns && parameters.matchPatterns->isEmpty())
            return invalid("matches"_s, "it must not be empty"_s);
        if (registration && (!parameters.js || parameters.js->isEmpty()) && (!parameters.css || parameters.css->isEmpty()))
            return invalid("details"_s, "it must specify at least one 'css' or 'js' file"_s);

        if (script->getValue("runAt"_s)) {
            auto value = script->getString("runAt"_s);
            if (value == "document_start"_s)
                parameters.injectionTime = WebExtension::InjectionTime::DocumentStart;
            else if (value == "document_end"_s)
                parameters.injectionTime = WebExtension::InjectionTime::DocumentEnd;
            else if (value == "document_idle"_s)
                parameters.injectionTime = WebExtension::InjectionTime::DocumentIdle;
            else
                return invalid("runAt"_s, "it must specify either 'document_start', 'document_end', or 'document_idle'"_s);
        }
        if (script->getValue("cssOrigin"_s)) {
            auto value = script->getString("cssOrigin"_s);
            if (equalIgnoringASCIICase(value, "user"_s))
                parameters.styleLevel = WebCore::UserStyleLevel::User;
            else if (equalIgnoringASCIICase(value, "author"_s))
                parameters.styleLevel = WebCore::UserStyleLevel::Author;
            else
                return invalid("cssOrigin"_s, "it must specify either 'author' or 'user'"_s);
        }
        if (script->getValue("world"_s)) {
            auto value = script->getString("world"_s);
            if (equalIgnoringASCIICase(value, "main"_s))
                parameters.world = WebExtensionContentWorldType::Main;
            else if (equalIgnoringASCIICase(value, "isolated"_s))
                parameters.world = WebExtensionContentWorldType::ContentScript;
            else
                return invalid("world"_s, "it must specify either 'isolated' or 'main'"_s);
        }
        parameters.allFrames = script->getBoolean("allFrames"_s);
        if (auto matchParent = script->getBoolean("matchOriginAsFallback"_s))
            parameters.matchParentFrame = *matchParent ? WebCore::UserContentMatchParentFrame::ForOpaqueOrigins : WebCore::UserContentMatchParentFrame::Never;
        parameters.persistent = script->getBoolean("persistAcrossSessions"_s);
        if (registration && !parameters.persistent)
            parameters.persistent = true;
        result.append(WTF::move(parameters));
    }
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
