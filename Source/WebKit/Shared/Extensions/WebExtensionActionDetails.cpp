/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionActionDetails.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <cmath>
#include <wtf/text/StringToIntegerConversion.h>
namespace WebKit {
bool isValidWebExtensionActionIconDimension(const String& key)
{
    auto size = parseInteger<uint32_t>(key);
    return size && *size && String::number(*size) == key;
}

std::expected<RefPtr<JSON::Object>, String> parseWebExtensionActionIconJSON(const String& source)
{
    if (source.isNull())
        return RefPtr<JSON::Object> { };
    if (source.length() > maximumWebExtensionActionIconJSONLength)
        return std::unexpected("icon data exceeds the supported message size"_s);
    RefPtr value = JSON::Value::parseJSON(source);
    if (!value)
        return std::unexpected("invalid icon JSON"_s);
    if (value->type() == JSON::Value::Type::Null)
        return RefPtr<JSON::Object> { };
    RefPtr object = value->asObject();
    if (!object || object->size() > maximumWebExtensionActionIconRepresentations)
        return std::unexpected("icons must be a bounded size dictionary"_s);
    for (auto& key : object->keys()) {
        auto path = object->getString(key);
        if (!isValidWebExtensionActionIconDimension(key) || path.isEmpty() || path.contains('\0')
            || (!path.startsWith('/') && !path.startsWith("data:"_s)))
            return std::unexpected("invalid icon dimension or extension resource path"_s);
    }
    return object->size() ? object : RefPtr<JSON::Object> { };
}

std::expected<std::optional<WebExtensionTabIdentifier>, String> parseWebExtensionActionTab(const JSON::Value* value)
{
    if (!value)
        return std::optional<WebExtensionTabIdentifier> { };
    auto number = value->asDouble();
    auto tab = number ? toWebExtensionTabIdentifier(*number) : std::nullopt;
    if (!isValid(tab) || *number > 9007199254740991.0)
        return std::unexpected("tabId must be a valid safe integer identifier"_s);
    return tab;
}

std::expected<WebExtensionActionDetails, String> parseWebExtensionActionDetails(const JSON::Value* value, WebExtensionActionDetailsKind kind)
{
    WebExtensionActionDetails result;
    if ((!value || value->type() == JSON::Value::Type::Null) && kind == WebExtensionActionDetailsKind::Get)
        return result;
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return std::unexpected("action details must be an object"_s);
    const bool isColor = kind == WebExtensionActionDetailsKind::BadgeBackgroundColor || kind == WebExtensionActionDetailsKind::BadgeTextColor;
    ASCIILiteral valueKey = isColor ? "color"_s : kind == WebExtensionActionDetailsKind::Title ? "title"_s
        : kind == WebExtensionActionDetailsKind::BadgeText ? "text"_s : "popup"_s;
    bool hasValue = false;
    for (auto& key : object->keys()) {
        Ref field = object->getValue(key).releaseNonNull();
        if (key == "tabId"_s) {
            auto tab = parseWebExtensionActionTab(field.ptr());
            if (!tab)
                return std::unexpected(tab.error());
            result.tab = *tab;
        } else if (key == "windowId"_s) {
            auto number = field->asDouble();
            auto window = number ? toWebExtensionWindowIdentifier(*number) : std::nullopt;
            if (!isValid(window) || *number > 9007199254740991.0)
                return std::unexpected("windowId must be a valid safe integer identifier"_s);
            result.window = window;
        } else if (kind != WebExtensionActionDetailsKind::Get && key == valueKey) {
            hasValue = true;
            if (field->type() == JSON::Value::Type::Null)
                result.value = nullString();
            else if (isColor && field->type() == JSON::Value::Type::Array) {
                Ref array = field->asArray().releaseNonNull();
                if (array->length() != 4)
                    return std::unexpected("action color must contain four RGBA bytes"_s);
                uint32_t packed = 0;
                for (auto& component : array.get()) {
                    auto number = component->asDouble();
                    if (!number || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0 || *number > 255)
                        return std::unexpected("action color components must be integers from 0 to 255"_s);
                    packed = (packed << 8) | static_cast<uint32_t>(*number);
                }
                result.color = packed;
            } else if (field->type() == JSON::Value::Type::String) {
                result.value = field->asString();
                if (!isColor && result.value.contains('\0'))
                    return std::unexpected("action text must not contain null characters"_s);
            } else
                return std::unexpected(isColor ? "action color must be CSS text, an RGBA array or null"_s : "action text must be a string or null"_s);
        } else
            return std::unexpected("unsupported action detail"_s);
    }
    if (result.tab && result.window)
        return std::unexpected("action details cannot specify both tabId and windowId"_s);
    if (kind != WebExtensionActionDetailsKind::Get && kind != WebExtensionActionDetailsKind::BadgeTextColor && !hasValue)
        return std::unexpected(isColor ? "the action color field is required"_s : "the action text field is required"_s);
    return result;
}
}
#endif
