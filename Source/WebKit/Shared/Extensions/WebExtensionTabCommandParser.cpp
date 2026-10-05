/*
 * Copyright (C) 2022-2024 Apple Inc. All rights reserved.
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
#include "WebExtensionTabCommandParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionFrameIdentifier.h"
#include "WebExtensionUtilities.h"
#include <cmath>
#include <wtf/UUID.h>

namespace WebKit {

static constexpr double maxSafeInteger = 9007199254740991.0;

static std::unexpected<String> invalid(const String& key, const String& reason)
{
    return makeUnexpected(toErrorString(nullString(), key, reason));
}

// JSON conversion already dropped undefined members and functions.
static RefPtr<JSON::Value> member(const JSON::Object& object, const String& key)
{
    RefPtr value = object.getValue(key);
    return value && !value->isNull() ? value : nullptr;
}

static std::expected<std::optional<bool>, String> booleanMember(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return std::optional<bool> { };
    auto result = value->asBoolean();
    if (!result)
        return invalid(key, "a boolean is expected"_s);
    return result;
}

static std::expected<std::optional<double>, String> numberMember(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return std::optional<double> { };
    auto result = value->asDouble();
    if (!result || !std::isfinite(*result))
        return invalid(key, "a number is expected"_s);
    return result;
}

static std::expected<String, String> stringMember(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return String { };
    if (value->type() != JSON::Value::Type::String)
        return invalid(key, "a string is expected"_s);
    auto result = value->asString();
    if (result.contains('\0'))
        return invalid(key, "it contains a NUL character"_s);
    // A present but empty string stays distinguishable from a missing one.
    return result.isNull() ? emptyString() : result;
}

static std::expected<URL, String> resolvedURL(const String& key, const String& text, const URL& baseURL)
{
    URL url { baseURL, text };
    if (!url.isValid())
        return invalid(key, makeString('\'', text, "' is not a valid URL"_s));
    return url;
}

static std::expected<void, String> parseTabUpdateMembers(const JSON::Object& object, const URL& baseURL, WebExtensionTabParameters& parameters)
{
    auto url = stringMember(object, "url"_s);
    if (!url)
        return makeUnexpected(url.error());
    if (!url->isNull()) {
        auto resolved = resolvedURL("url"_s, *url, baseURL);
        if (!resolved)
            return makeUnexpected(resolved.error());
        parameters.url = WTF::move(*resolved);
    }

    auto openerTabId = numberMember(object, "openerTabId"_s);
    if (!openerTabId)
        return makeUnexpected(openerTabId.error());
    if (*openerTabId) {
        parameters.parentTabIdentifier = toWebExtensionTabIdentifier(**openerTabId);
        if (!isValid(parameters.parentTabIdentifier) || **openerTabId > maxSafeInteger)
            return invalid("openerTabId"_s, makeString('\'', **openerTabId, "' is not a tab identifier"_s));
    }

    struct Flag {
        ASCIILiteral key;
        std::optional<bool>& target;
    };
    // 'highlighted' is the modern spelling of 'selected' and wins when both appear.
    for (auto flag : { Flag { "active"_s, parameters.active }, Flag { "pinned"_s, parameters.pinned }, Flag { "muted"_s, parameters.muted },
        Flag { "selected"_s, parameters.selected }, Flag { "highlighted"_s, parameters.selected } }) {
        auto value = booleanMember(object, flag.key);
        if (!value)
            return makeUnexpected(value.error());
        if (*value)
            flag.target = **value;
    }
    return { };
}

std::expected<WebExtensionTabParameters, String> parseWebExtensionTabUpdateProperties(const JSON::Value* input, const URL& baseURL)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return invalid("properties"_s, "an object is expected"_s);
    WebExtensionTabParameters parameters;
    if (auto result = parseTabUpdateMembers(*object, baseURL, parameters); !result)
        return makeUnexpected(result.error());
    return parameters;
}

std::expected<WebExtensionTabParameters, String> parseWebExtensionTabCreateProperties(const JSON::Value* input, const URL& baseURL)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return invalid("properties"_s, "an object is expected"_s);
    WebExtensionTabParameters parameters;
    if (auto result = parseTabUpdateMembers(*object, baseURL, parameters); !result)
        return makeUnexpected(result.error());

    auto windowId = numberMember(*object, "windowId"_s);
    if (!windowId)
        return makeUnexpected(windowId.error());
    if (*windowId) {
        parameters.windowIdentifier = toWebExtensionWindowIdentifier(**windowId);
        if (!parameters.windowIdentifier || !isValid(*parameters.windowIdentifier) || **windowId > maxSafeInteger)
            return invalid("windowId"_s, makeString('\'', **windowId, "' is not a window identifier"_s));
    }

    auto index = numberMember(*object, "index"_s);
    if (!index)
        return makeUnexpected(index.error());
    if (*index)
        parameters.index = static_cast<uint64_t>(std::max(0.0, std::min(std::trunc(**index), maxSafeInteger)));

    auto readerMode = booleanMember(*object, "openInReaderMode"_s);
    if (!readerMode)
        return makeUnexpected(readerMode.error());
    if (*readerMode)
        parameters.showingReaderMode = **readerMode;

    // Firefox accepts a placeholder title for discarded tabs; validate and ignore it.
    if (auto title = stringMember(*object, "title"_s); !title)
        return makeUnexpected(title.error());
    return parameters;
}

std::expected<Vector<WebExtensionTabIdentifier>, String> parseWebExtensionTabIdentifiers(const JSON::Value* input)
{
    Vector<WebExtensionTabIdentifier> identifiers;
    auto append = [&](JSON::Value& value) -> std::expected<void, String> {
        auto number = value.asDouble();
        auto identifier = number ? toWebExtensionTabIdentifier(*number) : std::nullopt;
        if (!number || !isValid(identifier) || *number > maxSafeInteger)
            return invalid("tabIDs"_s, makeString('\'', value.toJSONString(), "' is not a tab identifier"_s));
        identifiers.append(*identifier);
        return { };
    };
    if (!input)
        return invalid("tabIDs"_s, "a number or an array of numbers is expected"_s);
    // JSON::Value only offers a mutable array view; nothing here modifies it.
    if (RefPtr array = const_cast<JSON::Value*>(input)->asArray()) {
        if (array->length() > 4096)
            return invalid("tabIDs"_s, "it names too many tabs"_s);
        for (auto& value : *array) {
            if (auto result = append(value); !result)
                return makeUnexpected(result.error());
        }
        return identifiers;
    }
    if (input->type() != JSON::Value::Type::Double && input->type() != JSON::Value::Type::Integer)
        return invalid("tabIDs"_s, "a number or an array of numbers is expected"_s);
    if (auto result = append(const_cast<JSON::Value&>(*input)); !result)
        return makeUnexpected(result.error());
    return identifiers;
}

std::expected<bool, String> parseWebExtensionTabReloadProperties(const JSON::Value* input)
{
    if (!input || input->isNull())
        return false;
    RefPtr object = input->asObject();
    if (!object)
        return invalid("properties"_s, "an object is expected"_s);
    auto bypassCache = booleanMember(*object, "bypassCache"_s);
    if (!bypassCache)
        return makeUnexpected(bypassCache.error());
    return bypassCache->value_or(false);
}

std::expected<WebExtensionScriptInjectionParameters, String> parseWebExtensionTabCSSDetails(const JSON::Value* input)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return invalid("details"_s, "an object is expected"_s);

    WebExtensionScriptInjectionParameters parameters;
    auto file = stringMember(*object, "file"_s);
    if (!file)
        return makeUnexpected(file.error());
    auto code = stringMember(*object, "code"_s);
    if (!code)
        return makeUnexpected(code.error());
    if (!file->isNull() && !code->isNull())
        return invalid("details"_s, "it cannot specify both 'file' and 'code'"_s);
    if (file->isNull() && code->isNull())
        return invalid("details"_s, "it must specify either 'file' or 'code'"_s);
    if (!file->isNull()) {
        if (file->isEmpty())
            return invalid("file"_s, "it cannot be empty"_s);
        parameters.files = Vector<String> { *file };
    } else
        parameters.code = *code;

    auto allFrames = booleanMember(*object, "allFrames"_s);
    if (!allFrames)
        return makeUnexpected(allFrames.error());
    auto frameId = numberMember(*object, "frameId"_s);
    if (!frameId)
        return makeUnexpected(frameId.error());
    auto documentId = stringMember(*object, "documentId"_s);
    if (!documentId)
        return makeUnexpected(documentId.error());
    bool everyFrame = allFrames->value_or(false);
    if (everyFrame && *frameId)
        return invalid("details"_s, "it cannot specify both 'allFrames' and 'frameId'"_s);
    if (*frameId && !documentId->isNull())
        return invalid("details"_s, "it cannot specify both 'frameId' and 'documentId'"_s);
    if (everyFrame && !documentId->isNull())
        return invalid("details"_s, "it cannot specify both 'allFrames' and 'documentId'"_s);

    if (!documentId->isNull()) {
        auto identifier = WTF::UUID::parse(*documentId);
        if (!identifier)
            return invalid("documentId"_s, makeString('\'', *documentId, "' is not a valid document identifier"_s));
        parameters.documentIdentifiers = Vector<WTF::UUID> { *identifier };
    }
    if (*frameId) {
        auto identifier = toWebExtensionFrameIdentifier(**frameId);
        if (!isValid(identifier))
            return invalid("frameId"_s, makeString('\'', **frameId, "' is not a frame identifier"_s));
        parameters.frameIdentifiers = Vector<WebExtensionFrameIdentifier> { *identifier };
    } else if (!everyFrame && !parameters.documentIdentifiers)
        parameters.frameIdentifiers = Vector<WebExtensionFrameIdentifier> { WebExtensionFrameConstants::MainFrameIdentifier };

    auto origin = stringMember(*object, "cssOrigin"_s);
    if (!origin)
        return makeUnexpected(origin.error());
    if (!origin->isNull()) {
        if (equalLettersIgnoringASCIICase(*origin, "user"_s))
            parameters.styleLevel = WebCore::UserStyleLevel::User;
        else if (equalLettersIgnoringASCIICase(*origin, "author"_s))
            parameters.styleLevel = WebCore::UserStyleLevel::Author;
        else
            return invalid("cssOrigin"_s, "it must specify either 'author' or 'user'"_s);
    }
    return parameters;
}

static std::expected<void, String> parseWindowUpdateMembers(const JSON::Object& object, const String& sourceKey, WebExtensionWindowParameters& parameters)
{
    auto state = stringMember(object, "state"_s);
    if (!state)
        return makeUnexpected(state.error());
    if (!state->isNull()) {
        if (*state == "normal"_s)
            parameters.state = WebExtensionWindow::State::Normal;
        else if (*state == "minimized"_s)
            parameters.state = WebExtensionWindow::State::Minimized;
        else if (*state == "maximized"_s)
            parameters.state = WebExtensionWindow::State::Maximized;
        else if (*state == "fullscreen"_s)
            parameters.state = WebExtensionWindow::State::Fullscreen;
        else
            return invalid("state"_s, "it must specify 'normal', 'minimized', 'maximized', or 'fullscreen'"_s);
    }

    auto focused = booleanMember(object, "focused"_s);
    if (!focused)
        return makeUnexpected(focused.error());
    if (*focused)
        parameters.focused = **focused;

    bool hasFrame = false;
    for (auto key : { "left"_s, "top"_s, "width"_s, "height"_s }) {
        auto value = numberMember(object, key);
        if (!value)
            return makeUnexpected(value.error());
        hasFrame |= value->has_value();
    }
    if (hasFrame && parameters.state && *parameters.state != WebExtensionWindow::State::Normal)
        return invalid(sourceKey, "when 'top', 'left', 'width', or 'height' are specified, 'state' must specify 'normal'"_s);
    return { };
}

std::expected<WebExtensionWindowParameters, String> parseWebExtensionWindowUpdateInfo(const JSON::Value* input)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return invalid("properties"_s, "an object is expected"_s);
    WebExtensionWindowParameters parameters;
    if (auto result = parseWindowUpdateMembers(*object, "properties"_s, parameters); !result)
        return makeUnexpected(result.error());
    return parameters;
}

std::expected<WebExtensionWindowParameters, String> parseWebExtensionWindowCreateData(const JSON::Value* input, const URL& baseURL)
{
    WebExtensionWindowParameters parameters;
    if (!input || input->isNull())
        return parameters;
    RefPtr object = input->asObject();
    if (!object)
        return invalid("info"_s, "an object is expected"_s);
    if (auto result = parseWindowUpdateMembers(*object, "info"_s, parameters); !result)
        return makeUnexpected(result.error());

    auto type = stringMember(*object, "type"_s);
    if (!type)
        return makeUnexpected(type.error());
    if (!type->isNull()) {
        // Firefox's 'panel' and 'detached_panel' are aliases of a popup window.
        if (*type == "normal"_s)
            parameters.type = WebExtensionWindow::Type::Normal;
        else if (*type == "popup"_s || *type == "panel"_s || *type == "detached_panel"_s)
            parameters.type = WebExtensionWindow::Type::Popup;
        else
            return invalid("type"_s, "it must specify either 'normal' or 'popup'"_s);
    }

    auto incognito = booleanMember(*object, "incognito"_s);
    if (!incognito)
        return makeUnexpected(incognito.error());
    if (*incognito)
        parameters.privateBrowsing = **incognito;

    if (RefPtr urls = member(*object, "url"_s)) {
        Vector<WebExtensionTabParameters> tabs;
        auto append = [&](const JSON::Value& value) -> std::expected<void, String> {
            if (value.type() != JSON::Value::Type::String)
                return invalid("url"_s, "a string or an array of strings is expected"_s);
            auto resolved = resolvedURL("url"_s, value.asString(), baseURL);
            if (!resolved)
                return makeUnexpected(resolved.error());
            WebExtensionTabParameters tab;
            tab.url = WTF::move(*resolved);
            tabs.append(WTF::move(tab));
            return { };
        };
        if (RefPtr array = urls->asArray()) {
            if (array->length() > 512)
                return invalid("url"_s, "it names too many pages"_s);
            for (auto& value : *array) {
                if (auto result = append(value); !result)
                    return makeUnexpected(result.error());
            }
        } else if (auto result = append(*urls); !result)
            return makeUnexpected(result.error());
        parameters.tabs = WTF::move(tabs);
    }

    auto tabId = numberMember(*object, "tabId"_s);
    if (!tabId)
        return makeUnexpected(tabId.error());
    if (*tabId) {
        auto identifier = toWebExtensionTabIdentifier(**tabId);
        if (!isValid(identifier) || **tabId > maxSafeInteger)
            return invalid("tabId"_s, makeString('\'', **tabId, "' is not a tab identifier"_s));
        if (!parameters.tabs)
            parameters.tabs = Vector<WebExtensionTabParameters> { };
        WebExtensionTabParameters tab;
        tab.identifier = identifier;
        parameters.tabs->append(WTF::move(tab));
    }
    return parameters;
}

}
#endif
