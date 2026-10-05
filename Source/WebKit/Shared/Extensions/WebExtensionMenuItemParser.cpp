/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionMenuItemParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionMatchPattern.h"
#include <cmath>
#include <wtf/UUID.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
namespace WebKit {
static constexpr int64_t maximumSafeInteger = 9007199254740991;
static bool validText(const String& value) { return !value.isNull() && !value.contains('\0'); }

std::expected<String, String> encodeWebExtensionMenuIdentifier(const JSON::Value* value)
{
    if (value && value->type() == JSON::Value::Type::String) {
        auto string = value->asString();
        if (validText(string) && !string.isEmpty())
            return makeString("s:"_s, string);
    } else if (value) {
        if (auto number = value->asDouble(); number && std::isfinite(*number) && std::trunc(*number) == *number && std::abs(*number) <= maximumSafeInteger)
            return makeString("n:"_s, String::number(static_cast<int64_t>(*number)));
    }
    return std::unexpected("menu identifiers must be nonempty strings or safe integers"_s);
}

std::expected<WebExtensionMenuIdentifier, String> decodeWebExtensionMenuIdentifier(const String& key)
{
    if (key.startsWith("s:"_s)) {
        auto value = key.substring(2);
        if (validText(value) && !value.isEmpty())
            return WebExtensionMenuIdentifier { WTF::move(value) };
    } else if (key.startsWith("n:"_s)) {
        auto value = key.substring(2);
        auto number = parseInteger<int64_t>(value);
        if (number && *number >= -maximumSafeInteger && *number <= maximumSafeInteger && String::number(*number) == value)
            return WebExtensionMenuIdentifier { *number };
    }
    return std::unexpected("invalid native menu identifier"_s);
}

static bool validType(WebExtensionMenuItemType type)
{
    switch (type) {
    case WebExtensionMenuItemType::Normal:
    case WebExtensionMenuItemType::Checkbox:
    case WebExtensionMenuItemType::Radio:
    case WebExtensionMenuItemType::Separator:
        return true;
    }
    return false;
}

static std::expected<void, String> validatePatterns(const Vector<String>& patterns, bool documentPatterns)
{
    for (auto& text : patterns) {
        if (!validText(text))
            return std::unexpected("menu URL patterns must be strings without null characters"_s);
        RefPtr pattern = WebExtensionMatchPattern::getOrCreate(text);
        if (!pattern || (documentPatterns ? !pattern->isSupported() : !pattern->isValid()))
            return std::unexpected("invalid menu URL match pattern"_s);
    }
    return { };
}

static bool validDimension(const String& key)
{
    if (key == "any"_s)
        return true;
    auto size = parseInteger<uint32_t>(key);
    return size && *size && String::number(*size) == key;
}

static std::expected<String, String> parseIcons(const JSON::Value& value, const URL& baseURL)
{
    if (value.type() == JSON::Value::Type::Null)
        return emptyString();
    Ref result = JSON::Object::create();
    auto add = [&](const String& size, const JSON::Value& pathValue) -> bool {
        if (!validDimension(size) || pathValue.type() != JSON::Value::Type::String)
            return false;
        auto path = pathValue.asString();
        if (!validText(path) || path.isEmpty())
            return false;
        URL url { baseURL, path };
        if (!baseURL.isValid() || !url.isValid() || !protocolHostAndPortAreEqual(url, baseURL))
            return false;
        result->setString(size, url.path().toString());
        return true;
    };
    if (value.type() == JSON::Value::Type::String) {
        if (!add("16"_s, value))
            return std::unexpected("menu icons must name extension resources"_s);
    } else if (RefPtr object = value.asObject()) {
        for (auto& key : object->keys()) {
            if (!add(key, *object->getValue(key)))
                return std::unexpected("menu icon dimensions and resource paths must be valid"_s);
        }
    } else
        return std::unexpected("icons must be a resource path, dictionary or null"_s);
    return result->toJSONString();
}

std::expected<void, String> validateWebExtensionMenuItemParameters(const WebExtensionMenuItemParameters& parameters, WebExtensionMenuItemOperation operation)
{
    bool creating = operation == WebExtensionMenuItemOperation::Create;
    if ((creating || !parameters.identifier.isNull()) && !decodeWebExtensionMenuIdentifier(parameters.identifier))
        return std::unexpected("invalid menu identifier"_s);
    if (parameters.parentIdentifier && !decodeWebExtensionMenuIdentifier(*parameters.parentIdentifier))
        return std::unexpected("invalid parent menu identifier"_s);
    if (parameters.type && !validType(*parameters.type))
        return std::unexpected("invalid menu item type"_s);
    auto effectiveType = parameters.type.value_or(WebExtensionMenuItemType::Normal);
    if ((creating || parameters.type) && !isCheckedType(effectiveType) && parameters.checked.value_or(false))
        return std::unexpected("only checkbox and radio items can be checked"_s);
    if ((!parameters.title.isNull() && !validText(parameters.title)) || ((creating || parameters.type) && effectiveType != WebExtensionMenuItemType::Separator && parameters.title.isEmpty() && (creating || !parameters.title.isNull())))
        return std::unexpected("non-separator menu items require a nonempty title"_s);
    if (!parameters.command.isNull() && (!validText(parameters.command) || parameters.command.isEmpty()))
        return std::unexpected("menu command must be a nonempty string"_s);
    if (parameters.contexts && (parameters.contexts->isEmpty() || !allWebExtensionMenuItemContextTypes().containsAll(*parameters.contexts)))
        return std::unexpected("menu contexts must name supported contexts"_s);
    if (parameters.documentURLPatterns) {
        auto validation = validatePatterns(*parameters.documentURLPatterns, true);
        if (!validation)
            return validation;
    }
    if (parameters.targetURLPatterns) {
        auto validation = validatePatterns(*parameters.targetURLPatterns, false);
        if (!validation)
            return validation;
    }
    if (!parameters.iconsJSON.isEmpty()) {
        RefPtr value = JSON::Value::parseJSON(parameters.iconsJSON);
        RefPtr icons = value ? value->asObject() : nullptr;
        if (!icons)
            return std::unexpected("invalid native icon dictionary"_s);
        for (auto& key : icons->keys()) {
            auto path = icons->getString(key);
            if (!validDimension(key) || !validText(path) || !path.startsWith('/') || path.startsWith("//"_s))
                return std::unexpected("invalid native icon resource path"_s);
        }
    }
    return { };
}

std::expected<WebExtensionMenuItemParameters, String> parseWebExtensionMenuItemProperties(const JSON::Value* value, WebExtensionMenuItemOperation operation, bool manifestVersion3, const URL& baseURL)
{
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return std::unexpected("menu properties must be a dictionary"_s);
    WebExtensionMenuItemParameters result;
    for (auto& key : object->keys()) {
        Ref field = object->getValue(key).releaseNonNull();
        if (key == "id"_s || key == "parentId"_s) {
            auto identifier = encodeWebExtensionMenuIdentifier(field.ptr());
            if (!identifier)
                return std::unexpected(identifier.error());
            if (key == "id"_s)
                result.identifier = WTF::move(*identifier);
            else
                result.parentIdentifier = WTF::move(*identifier);
        } else if (key == "title"_s || key == "command"_s) {
            if (field->type() != JSON::Value::Type::String || !validText(field->asString()))
                return std::unexpected("menu title and command must be strings without null characters"_s);
            (key == "title"_s ? result.title : result.command) = field->asString();
        } else if (key == "checked"_s || key == "enabled"_s || key == "visible"_s) {
            auto boolean = field->asBoolean();
            if (!boolean)
                return std::unexpected("menu flags must be booleans"_s);
            (key == "checked"_s ? result.checked : key == "enabled"_s ? result.enabled : result.visible) = *boolean;
        } else if (key == "type"_s) {
            auto type = field->asString();
            if (type == "normal"_s) result.type = WebExtensionMenuItemType::Normal;
            else if (type == "checkbox"_s) result.type = WebExtensionMenuItemType::Checkbox;
            else if (type == "radio"_s) result.type = WebExtensionMenuItemType::Radio;
            else if (type == "separator"_s) result.type = WebExtensionMenuItemType::Separator;
            else return std::unexpected("unknown menu item type"_s);
        } else if (key == "contexts"_s) {
            RefPtr array = field->asArray();
            if (!array || !array->length())
                return std::unexpected("contexts must be a nonempty array"_s);
            OptionSet<WebExtensionMenuItemContextType> contexts;
            for (auto& entry : *array) {
                auto context = entry->asString();
                using Type = WebExtensionMenuItemContextType;
                if (context == "all"_s) contexts.add(allWebExtensionMenuItemContextTypes());
                else if (context == "action"_s && manifestVersion3) contexts.add(Type::Action);
                else if ((context == "browser_action"_s || context == "page_action"_s) && !manifestVersion3) contexts.add(Type::Action);
                else if (context == "audio"_s) contexts.add(Type::Audio);
                else if (context == "editable"_s) contexts.add(Type::Editable);
                else if (context == "frame"_s) contexts.add(Type::Frame);
                else if (context == "image"_s) contexts.add(Type::Image);
                else if (context == "link"_s) contexts.add(Type::Link);
                else if (context == "page"_s) contexts.add(Type::Page);
                else if (context == "selection"_s) contexts.add(Type::Selection);
                else if (context == "tab"_s) contexts.add(Type::Tab);
                else if (context == "video"_s) contexts.add(Type::Video);
                // Firefox distinguishes password fields, which are editable fields here.
                else if (context == "password"_s) contexts.add(Type::Editable);
                // Valid contexts of other browsers for surfaces this browser does not have.
                // An item naming them together with supported contexts remains usable.
                else if (context == "bookmark"_s || context == "tools_menu"_s || context == "launcher"_s) continue;
                else return std::unexpected("unsupported menu context for this manifest version"_s);
            }
            if (contexts.isEmpty())
                return std::unexpected("none of the requested menu contexts is available in this browser"_s);
            result.contexts = contexts;
        } else if (key == "documentUrlPatterns"_s || key == "targetUrlPatterns"_s) {
            RefPtr array = field->asArray();
            if (!array)
                return std::unexpected("menu URL patterns must be arrays of strings"_s);
            bool documentPatterns = key == "documentUrlPatterns"_s;
            Vector<String> patterns;
            bool droppedPattern = false;
            for (auto& entry : *array) {
                if (entry->type() != JSON::Value::Type::String)
                    return std::unexpected("menu URL patterns must be strings"_s);
                auto text = entry->asString();
                // Chrome and Firefox ignore patterns for schemes they cannot match (uBlock Origin
                // lists "abp:*" beside an https pattern), so drop them rather than the whole item.
                RefPtr pattern = validText(text) ? WebExtensionMatchPattern::getOrCreate(text) : nullptr;
                if (!pattern || (documentPatterns ? !pattern->isSupported() : !pattern->isValid())) {
                    droppedPattern = true;
                    continue;
                }
                patterns.append(WTF::move(text));
            }
            if (patterns.isEmpty() && droppedPattern)
                return std::unexpected("invalid menu URL match pattern"_s);
            (documentPatterns ? result.documentURLPatterns : result.targetURLPatterns) = WTF::move(patterns);
        } else if (key == "icons"_s) {
            auto icons = parseIcons(field.get(), baseURL);
            if (!icons)
                return std::unexpected(icons.error());
            result.iconsJSON = WTF::move(*icons);
        } else
            return std::unexpected("unsupported menu property"_s);
    }
    if (operation == WebExtensionMenuItemOperation::Create && result.identifier.isNull())
        result.identifier = makeString("s:"_s, createVersion4UUIDString());
    auto validation = validateWebExtensionMenuItemParameters(result, operation);
    if (!validation)
        return std::unexpected(validation.error());
    return result;
}
}
#endif
