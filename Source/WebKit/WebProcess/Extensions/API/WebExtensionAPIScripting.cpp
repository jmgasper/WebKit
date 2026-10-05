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
#include "WebExtensionAPIScripting.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionFrameIdentifier.h"
#include "WebExtensionRegisteredScriptParameters.h"
#include "WebExtensionRegisteredScriptParser.h"
#include "WebExtensionScriptInjectionParameters.h"
#include "WebExtensionTabIdentifier.h"
#include "WebExtensionTabScriptFrameResult.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"
#include <WebCore/SerializedScriptValue.h>
#include <WebCore/UserGestureIndicator.h>
#include <cmath>
#include <wtf/UUID.h>

// C++ port of Cocoa/WebExtensionAPIScriptingCocoa.mm. Script results come back
// as structured clones from the native tab-script executor, not as JSON.

namespace WebKit {

static std::unexpected<String> invalid(const String& key, const String& reason)
{
    return makeUnexpected(toErrorString(nullString(), key, reason));
}

static RefPtr<JSON::Value> member(const JSON::Object& object, const String& key)
{
    RefPtr value = object.getValue(key);
    return value && !value->isNull() ? value : nullptr;
}

static std::expected<std::optional<Vector<String>>, String> stringArrayMember(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return std::optional<Vector<String>> { };
    RefPtr array = value->asArray();
    if (!array)
        return invalid(key, "an array of strings is expected"_s);
    Vector<String> result;
    for (auto& item : *array) {
        if (item->type() != JSON::Value::Type::String || item->asString().contains('\0'))
            return invalid(key, "an array of strings is expected"_s);
        result.append(item->asString());
    }
    return std::optional { WTF::move(result) };
}

static std::expected<void, String> parseInjectionTarget(const JSON::Object& details, WebExtensionScriptInjectionParameters& parameters)
{
    RefPtr targetValue = member(details, "target"_s);
    RefPtr target = targetValue ? targetValue->asObject() : nullptr;
    if (!target)
        return invalid("details"_s, "it is missing the required 'target' object"_s);

    bool allFrames = false;
    if (RefPtr value = member(*target, "allFrames"_s)) {
        auto flag = value->asBoolean();
        if (!flag)
            return invalid("allFrames"_s, "a boolean is expected"_s);
        allFrames = *flag;
    }
    RefPtr frameIDs = member(*target, "frameIds"_s);
    RefPtr documentIDs = member(*target, "documentIds"_s);
    if (allFrames && frameIDs)
        return invalid("target"_s, "it cannot specify both 'allFrames' and 'frameIds'"_s);
    if (frameIDs && documentIDs)
        return invalid("target"_s, "it cannot specify both 'frameIds' and 'documentIds'"_s);
    if (allFrames && documentIDs)
        return invalid("target"_s, "it cannot specify both 'allFrames' and 'documentIds'"_s);

    RefPtr tabID = member(*target, "tabId"_s);
    auto tabNumber = tabID ? tabID->asDouble() : std::nullopt;
    auto tabIdentifier = tabNumber ? toWebExtensionTabIdentifier(*tabNumber) : std::nullopt;
    if (!isValid(tabIdentifier) || *tabNumber > 9007199254740991.0)
        return invalid("tabId"_s, "it is not a tab identifier"_s);
    parameters.tabIdentifier = tabIdentifier;

    if (documentIDs) {
        auto strings = stringArrayMember(*target, "documentIds"_s);
        if (!strings)
            return makeUnexpected(strings.error());
        Vector<WTF::UUID> identifiers;
        for (auto& string : **strings) {
            auto identifier = WTF::UUID::parse(string);
            if (!identifier)
                return invalid("documentIds"_s, makeString('\'', string, "' is not a document identifier"_s));
            identifiers.append(*identifier);
        }
        if (identifiers.isEmpty() || identifiers.size() > 1024)
            return invalid("documentIds"_s, "it must name between 1 and 1024 documents"_s);
        parameters.documentIdentifiers = WTF::move(identifiers);
    }

    if (frameIDs) {
        RefPtr array = frameIDs->asArray();
        if (!array)
            return invalid("frameIds"_s, "an array of numbers is expected"_s);
        Vector<WebExtensionFrameIdentifier> identifiers;
        for (auto& item : *array) {
            auto number = item->asDouble();
            auto identifier = number ? toWebExtensionFrameIdentifier(*number) : std::nullopt;
            if (!isValid(identifier))
                return invalid("frameIds"_s, makeString('\'', item->toJSONString(), "' is not a frame identifier"_s));
            identifiers.append(*identifier);
        }
        if (identifiers.isEmpty() || identifiers.size() > 1024)
            return invalid("frameIds"_s, "it must name between 1 and 1024 frames"_s);
        parameters.frameIdentifiers = WTF::move(identifiers);
    } else if (!allFrames && !parameters.documentIdentifiers)
        parameters.frameIdentifiers = Vector<WebExtensionFrameIdentifier> { WebExtensionFrameConstants::MainFrameIdentifier };

    return { };
}

static std::expected<WebExtensionScriptInjectionParameters, String> parseCSSInjection(const JSON::Value* input)
{
    RefPtr details = input ? input->asObject() : nullptr;
    if (!details)
        return invalid("details"_s, "an object is expected"_s);
    WebExtensionScriptInjectionParameters parameters;
    if (auto result = parseInjectionTarget(*details, parameters); !result)
        return makeUnexpected(result.error());

    RefPtr css = member(*details, "css"_s);
    auto files = stringArrayMember(*details, "files"_s);
    if (!files)
        return makeUnexpected(files.error());
    if (css && *files)
        return invalid("details"_s, "it cannot specify both 'css' and 'files'"_s);
    if (!css && !*files)
        return invalid("details"_s, "it must specify either 'css' or 'files'"_s);
    if (css) {
        if (css->type() != JSON::Value::Type::String)
            return invalid("css"_s, "a string is expected"_s);
        parameters.css = css->asString();
    } else {
        if ((*files)->isEmpty())
            return invalid("files"_s, "at least one file must be specified"_s);
        parameters.files = WTF::move(**files);
    }

    if (RefPtr origin = member(*details, "origin"_s)) {
        auto text = origin->asString();
        if (equalLettersIgnoringASCIICase(text, "user"_s))
            parameters.styleLevel = WebCore::UserStyleLevel::User;
        else if (equalLettersIgnoringASCIICase(text, "author"_s))
            parameters.styleLevel = WebCore::UserStyleLevel::Author;
        else
            return invalid("origin"_s, "it must specify either 'author' or 'user'"_s);
    }
    return parameters;
}

static JSValueRef propertyOf(JSContextRef context, JSObjectRef object, ASCIILiteral name)
{
    JSRetainPtr key = adopt(JSStringCreateWithUTF8CString(name.characters()));
    JSValueRef exception = nullptr;
    // This is a safer cpp false positive (rdar://163760990).
    SUPPRESS_UNCOUNTED_ARG JSValueRef value = JSObjectGetProperty(context, object, key.get(), &exception);
    return exception || !value || JSValueIsUndefined(context, value) || JSValueIsNull(context, value) ? nullptr : value;
}

RefPtr<JSON::Object> WebExtensionAPIScripting::executionWorlds()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/ExecutionWorld

    Ref worlds = JSON::Object::create();
    worlds->setString("ISOLATED"_s, "ISOLATED"_s);
    worlds->setString("MAIN"_s, "MAIN"_s);
    return worlds;
}

void WebExtensionAPIScripting::executeScript(JSContextRef context, JSValueRef detailsValue, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/executeScript

    RefPtr json = isDictionary(context, detailsValue) ? toJSONValue(context, detailsValue) : nullptr;
    RefPtr details = json ? json->asObject() : nullptr;
    if (!details) {
        exception = toErrorString(nullString(), "details"_s, "an object is expected"_s);
        return;
    }

    WebExtensionScriptInjectionParameters parameters;
    if (auto result = parseInjectionTarget(*details, parameters); !result) {
        exception = result.error();
        return;
    }

    // Functions do not survive the JSON conversion above; read them directly.
    JSObjectRef detailsObject = JSValueToObject(context, detailsValue, nullptr);
    JSValueRef func = detailsObject ? propertyOf(context, detailsObject, "func"_s) : nullptr;
    JSValueRef function = detailsObject ? propertyOf(context, detailsObject, "function"_s) : nullptr;
    if (func && function) {
        exception = toErrorString(nullString(), "details"_s, "it cannot specify both 'func' and 'function'. Please use 'func'"_s);
        return;
    }
    auto usedFunctionKey = function ? "function"_s : "func"_s;
    if (function)
        func = function;

    RefPtr args = member(*details, "args"_s);
    RefPtr arguments = member(*details, "arguments"_s);
    if (args && arguments) {
        exception = toErrorString(nullString(), "details"_s, "it cannot specify both 'args' and 'arguments'. Please use 'args'"_s);
        return;
    }
    if (arguments)
        args = arguments;

    auto files = stringArrayMember(*details, "files"_s);
    if (!files) {
        exception = files.error();
        return;
    }
    if (*files && func) {
        exception = toErrorString(nullString(), "details"_s, "it cannot specify both 'files' and 'func'"_s);
        return;
    }
    if (!*files && !func) {
        exception = toErrorString(nullString(), "details"_s, "it must specify either 'func' or 'files'"_s);
        return;
    }
    if (args && !func) {
        exception = toErrorString(nullString(), "details"_s, "it must specify both 'func' and 'args'"_s);
        return;
    }

    if (RefPtr world = member(*details, "world"_s)) {
        auto text = world->asString();
        if (equalLettersIgnoringASCIICase(text, "main"_s))
            parameters.world = WebExtensionContentWorldType::Main;
        else if (equalLettersIgnoringASCIICase(text, "isolated"_s))
            parameters.world = WebExtensionContentWorldType::ContentScript;
        else {
            exception = toErrorString(nullString(), "world"_s, "it must specify either 'ISOLATED' or 'MAIN'"_s);
            return;
        }
    }

    bool injectImmediately = false;
    if (RefPtr value = member(*details, "injectImmediately"_s)) {
        auto flag = value->asBoolean();
        if (!flag) {
            exception = toErrorString(nullString(), "injectImmediately"_s, "a boolean is expected"_s);
            return;
        }
        injectImmediately = *flag;
    }

    if (*files) {
        if ((*files)->isEmpty()) {
            exception = toErrorString(nullString(), "files"_s, "at least one file must be specified"_s);
            return;
        }
        parameters.files = WTF::move(**files);
    } else {
        if (!isFunction(context, func)) {
            exception = toErrorString(nullString(), usedFunctionKey, "it is not a function"_s);
            return;
        }
        RefPtr argumentArray = args ? args->asArray() : nullptr;
        if (args && !argumentArray) {
            exception = toErrorString(nullString(), "args"_s, "an array is expected"_s);
            return;
        }
        // A function cannot be transferred across processes, so it travels as
        // source text applied to its JSON-serialized arguments.
        auto source = toString(context, func);
        auto argumentsJSON = argumentArray ? argumentArray->toJSONString() : "[]"_s;
        parameters.code = makeString('(', source, ").apply(null, "_s, argumentsJSON, ')');
        if (parameters.code->length() > webExtensionMaxMessageLength) {
            exception = toErrorString(nullString(), usedFunctionKey, "it exceeded the maximum allowed length"_s);
            return;
        }
    }

    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingExecuteScriptNative(WTF::move(parameters), injectImmediately, userGesture),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionTabScriptFrameResult>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Vector<Protected<JSValueRef>> entries;
            for (auto& frame : *result) {
                HashMap<String, Protected<JSValueRef>> entry;
                entry.set("frameId"_s, Protected<JSValueRef>(context, JSValueMakeNumber(context, toWebAPI(frame.frameIdentifier))));
                if (frame.documentIdentifier)
                    entry.set("documentId"_s, Protected<JSValueRef>(context, toJSValueRef(context, frame.documentIdentifier->toString())));
                JSValueRef cloneException = nullptr;
                JSValueRef value = frame.error.isNull() && frame.value ? frame.value->deserialize(context, &cloneException) : nullptr;
                if (cloneException)
                    value = nullptr;
                // Chrome and Firefox report a frame without a usable value as null.
                entry.set("result"_s, Protected<JSValueRef>(context, value ?: JSValueMakeNull(context)));
                if (!frame.error.isNull())
                    entry.set("error"_s, Protected<JSValueRef>(context, toJSValueRef(context, frame.error)));
                else if (cloneException)
                    entry.set("error"_s, Protected<JSValueRef>(context, toJSValueRef(context, "the script result could not be cloned into this extension"_s)));
                entries.append(Protected<JSValueRef>(context, fromObject(context, WTF::move(entry))));
            }
            Protected<JSValueRef> array(context, fromArray(context, WTF::move(entries)));
            callback->call(array.get());
        }, extensionContext().identifier());
}

void WebExtensionAPIScripting::insertCSS(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/insertCSS

    auto parameters = parseCSSInjection(details.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingInsertCSS(WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

void WebExtensionAPIScripting::removeCSS(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/removeCSS

    auto parameters = parseCSSInjection(details.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingRemoveCSS(WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

static std::expected<Vector<WebExtensionRegisteredScriptParameters>, String> parseRegisteredScripts(const JSON::Value* input, WebExtensionRegisteredScriptParseMode mode)
{
    // JSON::Value only offers a mutable array view; nothing here modifies it.
    RefPtr array = input ? const_cast<JSON::Value*>(input)->asArray() : nullptr;
    if (!array)
        return invalid("scripts"_s, "an array of objects is expected"_s);
    if (array->length() > 1024)
        return invalid("scripts"_s, "it names too many scripts"_s);
    Vector<Ref<JSON::Object>> scripts;
    for (auto& item : *array) {
        RefPtr object = item->asObject();
        if (!object)
            return invalid("scripts"_s, "an array of objects is expected"_s);
        scripts.append(object.releaseNonNull());
    }
    return parseWebExtensionRegisteredScripts(scripts, mode);
}

void WebExtensionAPIScripting::registerContentScripts(RefPtr<JSON::Value> scripts, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/registerContentScripts

    auto parameters = parseRegisteredScripts(scripts.get(), WebExtensionRegisteredScriptParseMode::Registration);
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingRegisterContentScripts(WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

void WebExtensionAPIScripting::updateContentScripts(RefPtr<JSON::Value> scripts, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/updateContentScripts

    auto parameters = parseRegisteredScripts(scripts.get(), WebExtensionRegisteredScriptParseMode::Update);
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingUpdateRegisteredScripts(WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

static std::expected<Vector<String>, String> parseScriptFilter(const JSON::Value* input)
{
    if (!input || input->isNull())
        return Vector<String> { };
    RefPtr filter = input->asObject();
    if (!filter)
        return invalid("filter"_s, "an object is expected"_s);
    auto identifiers = stringArrayMember(*filter, "ids"_s);
    if (!identifiers)
        return makeUnexpected(identifiers.error());
    return identifiers->value_or(Vector<String> { });
}

static Ref<JSON::Array> toJSONArray(const Vector<String>& strings)
{
    Ref array = JSON::Array::create();
    for (auto& string : strings)
        array->pushString(string);
    return array;
}

static Ref<JSON::Object> toWebAPI(const WebExtensionRegisteredScriptParameters& parameters)
{
    Ref result = JSON::Object::create();
    result->setString("id"_s, parameters.identifier);
    result->setArray("matches"_s, toJSONArray(parameters.matchPatterns.value_or(Vector<String> { })));
    result->setBoolean("persistAcrossSessions"_s, parameters.persistent.value_or(true));
    if (parameters.js)
        result->setArray("js"_s, toJSONArray(*parameters.js));
    if (parameters.css)
        result->setArray("css"_s, toJSONArray(*parameters.css));
    if (parameters.excludeMatchPatterns)
        result->setArray("excludeMatches"_s, toJSONArray(*parameters.excludeMatchPatterns));
    if (parameters.allFrames)
        result->setBoolean("allFrames"_s, *parameters.allFrames);
    if (parameters.matchParentFrame)
        result->setBoolean("matchOriginAsFallback"_s, *parameters.matchParentFrame == WebCore::UserContentMatchParentFrame::ForOpaqueOrigins);
    if (parameters.injectionTime) {
        switch (*parameters.injectionTime) {
        case WebExtension::InjectionTime::DocumentStart:
            result->setString("runAt"_s, "document_start"_s);
            break;
        case WebExtension::InjectionTime::DocumentEnd:
            result->setString("runAt"_s, "document_end"_s);
            break;
        case WebExtension::InjectionTime::DocumentIdle:
            result->setString("runAt"_s, "document_idle"_s);
            break;
        }
    }
    if (parameters.styleLevel)
        result->setString("cssOrigin"_s, *parameters.styleLevel == WebCore::UserStyleLevel::User ? "user"_s : "author"_s);
    if (parameters.world)
        result->setString("world"_s, *parameters.world == WebExtensionContentWorldType::Main ? "MAIN"_s : "ISOLATED"_s);
    return result;
}

void WebExtensionAPIScripting::getRegisteredContentScripts(RefPtr<JSON::Value> filter, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/getRegisteredContentScripts

    auto identifiers = parseScriptFilter(filter.get());
    if (!identifiers) {
        exception = identifiers.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingGetRegisteredScripts(WTF::move(*identifiers)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionRegisteredScriptParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            Ref scripts = JSON::Array::create();
            for (auto& parameters : *result)
                scripts->pushObject(toWebAPI(parameters));
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, fromJSON(context, RefPtr<JSON::Value> { WTF::move(scripts) }));
            callback->call(value.get());
        }, extensionContext().identifier());
}

void WebExtensionAPIScripting::unregisterContentScripts(RefPtr<JSON::Value> filter, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/scripting/unregisterContentScripts

    auto identifiers = parseScriptFilter(filter.get());
    if (!identifiers) {
        exception = identifiers.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ScriptingUnregisterContentScripts(WTF::move(*identifiers)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

}
#endif
