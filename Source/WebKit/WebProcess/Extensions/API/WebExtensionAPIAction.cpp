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
#include "WebExtensionAPIAction.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionActionDetails.h"
#include "JSWebExtensionString.h"
#include "JSWebExtensionTabParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionActionDetails.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/ArrayConstructor.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/PropertyNameArray.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <WebCore/Document.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/ImageData.h>
#include <WebCore/ImageUtilities.h>
#include <WebCore/JSImageData.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/UserGestureIndicator.h>
namespace WebKit {
void WebExtensionAPIAction::openPopup(WebPageProxyIdentifier page, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto target = parseWebExtensionActionDetails(options.get(), WebExtensionActionDetailsKind::Get);
    if (!target) {
        exception = target.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionOpenPopup(page, target->window, target->tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result)
                callback->reportError(result.error());
            else
                callback->call();
        }, extensionContext().identifier());
}

static std::expected<JSObjectRef, String> actionIconObject(JSContextRef context, JSValueRef value)
{
    if (!value || !JSValueIsObject(context, value))
        return std::unexpected("icon details must be a dictionary"_s);
    auto object = JSValueToObject(context, value, nullptr);
    if (JSObjectIsFunction(context, object))
        return std::unexpected("icon details must be a dictionary"_s);
    auto* global = toJS(context);
    JSC::JSLockHolder lock(global);
    auto& vm = global->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool array = JSC::isArray(global, toJS(global, value));
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, std::unexpected("icon dictionary type could not be read"_s));
        return std::unexpected("icon dictionary type could not be read"_s);
    }
    if (array)
        return std::unexpected("icon details must be a dictionary"_s);
    return object;
}

static std::expected<Vector<String>, String> actionIconPropertyNames(JSContextRef context, JSObjectRef object)
{
    auto* global = toJS(context);
    JSC::JSLockHolder lock(global);
    auto& vm = global->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSC::PropertyNameArrayBuilder names(vm, JSC::PropertyNameMode::Strings, JSC::PrivateSymbolMode::Exclude);
    // Own properties only, as in Chrome: some extensions add enumerable methods
    // to Object.prototype, and those are not icon sizes.
    auto* nativeObject = toJS(object);
    nativeObject->methodTable()->getOwnPropertyNames(nativeObject, global, names, JSC::DontEnumPropertiesMode::Exclude);
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, std::unexpected("icon dimensions could not be read"_s));
        return std::unexpected("icon dimensions could not be read"_s);
    }
    Vector<String> result;
    for (auto& name : names) {
        if (result.size() >= maximumWebExtensionActionIconRepresentations)
            return std::unexpected("too many icon representations"_s);
        result.append(name.string());
    }
    return result;
}

static RefPtr<WebCore::ImageData> actionImageData(JSContextRef context, JSValueRef value)
{
    auto* global = toJS(context);
    JSC::JSLockHolder lock(global);
    auto nativeValue = toJS(global, value);
    return nativeValue.isObject() ? WebCore::JSImageData::toWrapped(global->vm(), nativeValue.getObject()) : nullptr;
}

static std::expected<String, String> actionImageDataURL(WebCore::ImageData& image)
{
    // Retain the native ImageData and its buffer rather than consulting
    // replaceable JavaScript constructors, accessors or typed-array properties.
    constexpr size_t maximumImageBytes = 64 * 1024 * 1024;
    if (image.size().isEmpty() || image.data().isDetached() || image.data().byteLength() > maximumImageBytes
        || image.data().length() != uint64_t(image.width()) * image.height() * 4)
        return std::unexpected("ImageData is detached, invalid or too large"_s);
    Ref pixels = image.byteArrayPixelBuffer();
    RefPtr buffer = WebCore::ImageBuffer::create(image.size(), WebCore::RenderingMode::Unaccelerated,
        WebCore::RenderingPurpose::Unspecified, 1, WebCore::ColorSpace::SRGB(), WebCore::PixelFormat::BGRA8);
    if (!buffer)
        return std::unexpected("the icon image buffer could not be created"_s);
    buffer->putPixelBuffer(pixels.get(), WebCore::IntRect { { }, image.size() });
    auto encoded = WebCore::encodeDataURL(WTF::move(buffer), "image/png"_s);
    if (!encoded.startsWith("data:image/png;base64,"_s) || encoded.length() > maximumWebExtensionActionIconJSONLength)
        return std::unexpected("the icon ImageData could not be encoded"_s);
    return encoded;
}

static std::expected<String, String> actionIconPath(JSContextRef context, JSValueRef value, const URL& baseURL)
{
    if (!JSValueIsString(context, value))
        return std::unexpected("an icon path must be a string"_s);
    JSRetainPtr text(Adopt, JSValueToStringCopy(context, value, nullptr));
    auto path = toString(text.get());
    if (path.isEmpty() || path.contains('\0') || path.length() > maximumWebExtensionActionIconJSONLength)
        return std::unexpected("invalid icon path"_s);
    if (path.startsWithIgnoringASCIICase("data:"_s))
        return makeString("data:"_s, path.substring(5));
    URL url { baseURL, path };
    if (!baseURL.isValid() || !url.isValid() || !protocolHostAndPortAreEqual(url, baseURL))
        return std::unexpected("an icon path must name this extension's resources"_s);
    return url.path().toString();
}

static std::expected<Ref<JSON::Object>, String> actionIconRepresentations(JSContextRef context, JSValueRef value, bool imageData, const URL& baseURL)
{
    Ref icons = JSON::Object::create();
    size_t totalPathLength = 0;
    if (JSValueIsNull(context, value) || JSValueIsUndefined(context, value))
        return icons;
    auto add = [&](const String& dimension, JSValueRef source) -> std::expected<void, String> {
        if (!isValidWebExtensionActionIconDimension(dimension))
            return std::unexpected("icon dimensions must be positive integer keys"_s);
        std::expected<String, String> path = std::unexpected("an icon image must be an ImageData object"_s);
        if (imageData) {
            if (RefPtr image = actionImageData(context, source))
                path = actionImageDataURL(*image);
        } else
            path = actionIconPath(context, source, baseURL);
        if (!path)
            return std::unexpected(path.error());
        if (path->length() > maximumWebExtensionActionIconJSONLength - totalPathLength)
            return std::unexpected("icon data exceeds the supported message size"_s);
        totalPathLength += path->length();
        icons->setString(dimension, *path);
        return { };
    };
    if ((!imageData && JSValueIsString(context, value)) || (imageData && actionImageData(context, value))) {
        if (auto added = add("16"_s, value); !added)
            return std::unexpected(added.error());
        return icons;
    }
    auto object = actionIconObject(context, value);
    if (!object)
        return std::unexpected(object.error());
    Protected<JSObjectRef> protectedObject(JSContextGetGlobalContext(context), *object);
    auto names = actionIconPropertyNames(context, *object);
    if (!names)
        return std::unexpected(names.error());
    for (auto& name : *names) {
        JSValueRef exception = nullptr;
        Protected<JSValueRef> source(JSContextGetGlobalContext(context), JSObjectGetProperty(context, *object, toJSString(name).get(), &exception));
        if (exception)
            return std::unexpected("an icon property getter threw an exception"_s);
        if (auto added = add(name, source.get()); !added)
            return std::unexpected(added.error());
    }
    return icons;
}

void WebExtensionAPIAction::setIcon(WebFrame& frame, JSContextRef context, JSValueRef value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto object = actionIconObject(context, value);
    if (!object) {
        exception = object.error();
        return;
    }
    Protected<JSObjectRef> protectedObject(JSContextGetGlobalContext(context), *object);
    Ref scopes = JSON::Object::create();
    RefPtr<JSON::Object> icons;
    for (auto key : { "tabId"_s, "windowId"_s, "path"_s, "imageData"_s }) {
        JSValueRef getterException = nullptr;
        Protected<JSValueRef> field(JSContextGetGlobalContext(context), JSObjectGetProperty(context, *object, toJSString(key).get(), &getterException));
        if (getterException) {
            exception = "an icon detail getter threw an exception"_s;
            return;
        }
        if (JSValueIsUndefined(context, field.get()))
            continue;
        if (key == "tabId"_s || key == "windowId"_s) {
            if (!JSValueIsNumber(context, field.get())) {
                exception = "icon targets must be numeric identifiers"_s;
                return;
            }
            scopes->setDouble(key, JSValueToNumber(context, field.get(), nullptr));
            continue;
        }
        auto parsed = actionIconRepresentations(context, field.get(), key == "imageData"_s, frame.url());
        if (!parsed) {
            exception = parsed.error();
            return;
        }
        if (!(*parsed)->size())
            continue;
        // Given both, Chrome draws imageData and ignores path; Violentmonkey
        // passes both.
        if (icons && key != "imageData"_s)
            continue;
        icons = WTF::move(*parsed);
    }
    auto target = parseWebExtensionActionDetails(scopes.ptr(), WebExtensionActionDetailsKind::Get);
    if (!target) {
        exception = target.error();
        return;
    }
    auto iconsJSON = icons ? icons->toJSONString() : nullString();
    if (auto validated = parseWebExtensionActionIconJSON(iconsJSON); !validated) {
        exception = validated.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetIcon(target->window, target->tab, iconsJSON),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result)
                callback->reportError(result.error());
            else
                callback->call();
        }, extensionContext().identifier());
}

void WebExtensionAPIAction::getBadgeBackgroundColor(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    getBadgeColor(true, WTF::move(value), WTF::move(callback), exception);
}
void WebExtensionAPIAction::getBadgeTextColor(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    getBadgeColor(false, WTF::move(value), WTF::move(callback), exception);
}
void WebExtensionAPIAction::setBadgeBackgroundColor(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    setBadgeColor(true, WTF::move(value), WTF::move(callback), exception);
}
void WebExtensionAPIAction::setBadgeTextColor(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    setBadgeColor(false, WTF::move(value), WTF::move(callback), exception);
}
void WebExtensionAPIAction::getBadgeColor(bool background, RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Get);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionGetBadgeColor(details->window, details->tab, background),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<uint32_t, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            JSValueRef components[] = { JSValueMakeNumber(context, *result >> 24), JSValueMakeNumber(context, (*result >> 16) & 255),
                JSValueMakeNumber(context, (*result >> 8) & 255), JSValueMakeNumber(context, *result & 255) };
            Protected<JSValueRef> color(context, JSObjectMakeArray(context, 4, components, nullptr));
            callback->call(color.get());
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::setBadgeColor(bool background, RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), background ? WebExtensionActionDetailsKind::BadgeBackgroundColor : WebExtensionActionDetailsKind::BadgeTextColor);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetBadgeColor(details->window, details->tab, background, details->value, details->color),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result)
                callback->reportError(result.error());
            else
                callback->call();
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::getTitle(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Get);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionGetTitle(details->window, details->tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            Protected<JSValueRef> value(callback->globalContext(), JSValueMakeString(callback->globalContext(), toJSString(*result).get()));
            callback->call(value.get());
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::getBadgeText(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Get);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionGetBadgeText(details->window, details->tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            Protected<JSValueRef> value(callback->globalContext(), JSValueMakeString(callback->globalContext(), toJSString(*result).get()));
            callback->call(value.get());
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::getPopup(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Get);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionGetPopup(details->window, details->tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            Protected<JSValueRef> value(callback->globalContext(), JSValueMakeString(callback->globalContext(), toJSString(*result).get()));
            callback->call(value.get());
        }, extensionContext().identifier());
}
#if PLATFORM(HAIKU)
// Ghostery and other Chrome packages ask on every navigation whether their
// button is pinned; an absent function made them throw.
void WebExtensionAPIAction::getUserSettings(Ref<WebExtensionCallbackHandler>&& callback)
{
    auto context = callback->globalContext();
    JSObjectRef settings = JSObjectMake(context, nullptr, nullptr);
    JSObjectSetProperty(context, settings, toJSString("isOnToolbar"_s).get(), JSValueMakeBoolean(context, true), kJSPropertyAttributeNone, nullptr);
    callback->call(settings);
}
#endif

void WebExtensionAPIAction::isEnabled(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Get);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionGetEnabled(details->window, details->tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<bool, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            Protected<JSValueRef> value(callback->globalContext(), JSValueMakeBoolean(callback->globalContext(), *result));
            callback->call(value.get());
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::setTitle(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Title);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetTitle(details->window, details->tab, details->value),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::setBadgeText(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::BadgeText);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetBadgeText(details->window, details->tab, details->value),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::setPopup(RefPtr<JSON::Value> value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto details = parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Popup);
    if (!details) {
        exception = details.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetPopup(details->window, details->tab, details->value),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::enable(JSContextRef context, JSValueRef value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto tab = parseWebExtensionActionTabIdentifier(context, value);
    if (!tab) {
        exception = tab.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetEnabled(*tab, true),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result)
                callback->reportError(result.error());
            else
                callback->call();
        }, extensionContext().identifier());
}
void WebExtensionAPIAction::disable(JSContextRef context, JSValueRef value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto tab = parseWebExtensionActionTabIdentifier(context, value);
    if (!tab) {
        exception = tab.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ActionSetEnabled(*tab, false),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result)
                callback->reportError(result.error());
            else
                callback->call();
        }, extensionContext().identifier());
}
WebExtensionAPIEvent& WebExtensionAPIAction::onClicked()
{
    if (!m_onClicked)
        m_onClicked = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::ActionOnClicked);
    return *m_onClicked;
}
void WebExtensionContextProxy::dispatchActionClickedEvent(const std::optional<WebExtensionTabParameters>& tab)
{
    enumerateFramesAndNamespaceObjects([&](auto& frame, auto& object) {
        RefPtr coreFrame = frame.coreLocalFrame();
        WebCore::UserGestureIndicator gesture(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));
        object.action().onClicked().template invokeListenersWithArgumentFactory<1>([&](JSGlobalContextRef context) {
            return std::array { Protected<JSValueRef>(context, tab ? toWebAPI(context, *tab) : JSValueMakeUndefined(context)) };
        });
    });
}
}
#endif
