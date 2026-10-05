/*
 * Copyright (C) 2024 Igalia, S.L. All rights reserved.
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
#include "WebExtensionLocalization.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Logging.h"
#include "WebExtension.h"
#include "WebExtensionUtilities.h"
#include <JavaScriptCore/RegularExpression.h>
#include <wtf/Language.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

static constexpr auto messageKey = "message"_s;
static constexpr auto placeholdersKey = "placeholders"_s;
static constexpr auto placeholderDictionaryContentKey = "content"_s;

static constexpr auto predefinedMessageUILocale = "@@ui_locale"_s;
static constexpr auto predefinedMessageLanguageDirection = "@@bidi_dir"_s;
static constexpr auto predefinedMessageLanguageDirectionReversed = "@@bidi_reversed_dir"_s;
static constexpr auto predefinedMessageTextLeadingEdge = "@@bidi_start_edge"_s;
static constexpr auto predefinedMessageTextTrailingEdge = "@@bidi_end_edge"_s;
static constexpr auto predefinedMessageExtensionID = "@@extension_id"_s;

static constexpr auto predefinedMessageValueLeftToRight = "ltr"_s;
static constexpr auto predefinedMessageValueRightToLeft = "rtl"_s;
static constexpr auto predefinedMessageValueTextEdgeLeft = "left"_s;
static constexpr auto predefinedMessageValueTextEdgeRight = "right"_s;

WebExtensionLocalization::WebExtensionLocalization(WebExtension& webExtension)
{
    auto defaultLocaleString = webExtension.defaultLocale();
    if (defaultLocaleString.isEmpty()) {
        RELEASE_LOG_DEBUG(Extensions, "No default locale provided");
        loadRegionalLocalization(nullptr, nullptr, nullptr, emptyString(), emptyString());
        return;
    }

    RefPtr defaultLocaleJSON = localizationJSONForWebExtension(webExtension, defaultLocaleString);
    if (!defaultLocaleJSON) {
        RELEASE_LOG_DEBUG(Extensions, "No localization found for default locale %s", defaultLocaleString.utf8().legacyCStringPointer());
        loadRegionalLocalization(nullptr, nullptr, nullptr, emptyString(), emptyString());
        return;
    }

    RELEASE_LOG_DEBUG(Extensions, "Loaded default locale %s", defaultLocaleString.utf8().legacyCStringPointer());

    auto bestLocaleString = webExtension.bestMatchLocale();
    auto defaultLocaleComponents = parseLocale(defaultLocaleString);
    auto bestLocaleComponents = parseLocale(bestLocaleString);
    auto bestLocaleLanguageOnlyString = bestLocaleComponents.languageCode;

    RELEASE_LOG_DEBUG(Extensions, "Best locale is %s", bestLocaleString.utf8().legacyCStringPointer());

    RefPtr<JSON::Object> languageJSON;
    if (!bestLocaleLanguageOnlyString.isEmpty() && bestLocaleLanguageOnlyString != defaultLocaleString) {
        languageJSON = localizationJSONForWebExtension(webExtension, bestLocaleLanguageOnlyString);
        if (languageJSON)
            RELEASE_LOG_DEBUG(Extensions, "Loaded language-only locale %s", bestLocaleLanguageOnlyString.utf8().legacyCStringPointer());
    }

    RefPtr<JSON::Object> regionalJSON;
    if (bestLocaleString != bestLocaleLanguageOnlyString && bestLocaleString != defaultLocaleString) {
        regionalJSON = localizationJSONForWebExtension(webExtension, bestLocaleString);
        if (regionalJSON)
            RELEASE_LOG_DEBUG(Extensions, "Loaded regional locale %s", bestLocaleString.utf8().legacyCStringPointer());
    }

    loadRegionalLocalization(regionalJSON, languageJSON, defaultLocaleJSON, bestLocaleString);
}

WebExtensionLocalization::WebExtensionLocalization(RefPtr<JSON::Object> localizedJSON, const String& uniqueIdentifier)
{
    ASSERT(!uniqueIdentifier.isEmpty());
    ASSERT(localizedJSON);

    auto localeString = emptyString();
    if (RefPtr predefinedMessages = localizedJSON->getObject(predefinedMessageUILocale))
        localeString = predefinedMessages->getString(messageKey);

    loadRegionalLocalization(localizedJSON, nullptr, nullptr, localeString, uniqueIdentifier);
}

WebExtensionLocalization::WebExtensionLocalization(RefPtr<JSON::Object> regionalLocalization, RefPtr<JSON::Object> languageLocalization, RefPtr<JSON::Object> defaultLocalization, const String& bestLocale, const String& uniqueIdentifier)
{
    loadRegionalLocalization(regionalLocalization, languageLocalization, defaultLocalization, bestLocale, uniqueIdentifier);
}

void WebExtensionLocalization::loadRegionalLocalization(RefPtr<JSON::Object> regionalLocalization, RefPtr<JSON::Object> languageLocalization, RefPtr<JSON::Object> defaultLocalization, const String& bestLocale, const String& uniqueIdentifier)
{
    m_locale = WebCore::Locale::create(AtomString { bestLocale });
    m_localeString = bestLocale;
    m_uniqueIdentifier = uniqueIdentifier;

    RefPtr localizationJSON = predefinedMessages();
    localizationJSON = mergeJSON(localizationJSON, jsonWithLowercaseKeys(regionalLocalization));
    localizationJSON = mergeJSON(localizationJSON, jsonWithLowercaseKeys(languageLocalization));
    localizationJSON = mergeJSON(localizationJSON, jsonWithLowercaseKeys(defaultLocalization));

    ASSERT(localizationJSON);

    m_localizationJSON = localizationJSON;

    return;
}

RefPtr<JSON::Object> WebExtensionLocalization::localizedJSONforJSON(RefPtr<JSON::Object> json)
{
    if (!json)
        return nullptr;

    Ref newObject = JSON::Object::create();

    for (auto& key : json->keys()) {
        auto valueType = json->getValue(key)->type();

        if (valueType == JSON::Value::Type::String)
            newObject->setString(key, localizedStringForString(json->getString(key)));
        else if (valueType == JSON::Value::Type::Array)
            newObject->setArray(key, *localizedArrayForArray(json->getArray(key)));
        else if (valueType == JSON::Value::Type::Object)
            newObject->setObject(key, *localizedJSONforJSON(json->getObject(key)));
        else
            newObject->setValue(key, *json->getValue(key));
    }

    return newObject;
}

String WebExtensionLocalization::localizedStringForKey(String key, Vector<String> placeholders, bool escapeLessThan)
{
    if (placeholders.size() > 9)
        return emptyString();

    if (!m_localizationJSON || !m_localizationJSON->size())
        return emptyString();

    if (key.startsWith("__MSG_"_s) && key.endsWith("__"_s) && key.length() >= 8)
        key = key.substring(6, key.length() - 8);

    RefPtr localizationJSON = m_localizationJSON;
    if (!localizationJSON)
        return emptyString();

    RefPtr stringJSON = localizationJSON->getObject(key.convertToASCIILowercase());
    if (!stringJSON)
        return emptyString();

    auto localizedString = stringJSON->getString(messageKey);
    if (!localizedString.length())
        return emptyString();

    RefPtr namedPlaceholders = jsonWithLowercaseKeys(stringJSON->getObject(placeholdersKey));

    localizedString = stringByReplacingNamedPlaceholdersInString(localizedString, namedPlaceholders);
    // Escape translation text after named placeholder expansion, before
    // inserting the caller-provided positional substitutions.
    if (escapeLessThan)
        localizedString = makeStringByReplacingAll(localizedString, "<"_s, "&lt;"_s);
    localizedString = stringByReplacingPositionalPlaceholdersInString(localizedString, placeholders);

    return localizedString;
}

RefPtr<JSON::Array> WebExtensionLocalization::localizedArrayForArray(RefPtr<JSON::Array> json)
{
    if (!json)
        return nullptr;

    Ref newArray = JSON::Array::create();

    for (Ref value : *json) {
        auto valueType = value->type();

        if (valueType == JSON::Value::Type::String)
            newArray->pushString(localizedStringForString(value->asString()));
        else if (valueType == JSON::Value::Type::Array)
            newArray->pushArray(*localizedArrayForArray(value->asArray()));
        else if (valueType == JSON::Value::Type::Object)
            newArray->pushObject(*localizedJSONforJSON(value->asObject()));
        else
            newArray->pushValue(WTF::move(value));
    }

    return newArray;
}

String WebExtensionLocalization::localizedStringForString(String sourceString)
{
    StringBuilder result;
    auto localizableStringRegularExpression = JSC::Yarr::RegularExpression("__MSG_([A-Za-z0-9_@]+)__"_s, { });
    unsigned offset = 0;
    while (offset < sourceString.length()) {
        int matchLength;
        int index = localizableStringRegularExpression.match(sourceString, offset, &matchLength);
        if (index < 0)
            break;
        result.append(StringView { sourceString }.substring(offset, index - offset));
        result.append(localizedStringForKey(sourceString.substring(index, matchLength)));
        offset = index + matchLength;
    }
    result.append(StringView { sourceString }.substring(offset));
    return result.toString();
}

RefPtr<JSON::Object> WebExtensionLocalization::localizationJSONForWebExtension(WebExtension& webExtension, const String& locale)
{
    StringBuilder pathBuilder;
    pathBuilder.append("_locales/"_s);
    pathBuilder.append(locale);
    pathBuilder.append("/messages.json"_s);

    auto path = pathBuilder.toString();

    auto jsonStringResult = webExtension.resourceStringForPath(path, WebExtension::CacheResult::No, WebExtension::SuppressNotFoundErrors::Yes);
    if (!jsonStringResult) {
        webExtension.recordErrorIfNeeded(jsonStringResult.error());
        return nullptr;
    }

    RefPtr json = JSON::Value::parseJSON(jsonStringResult.value());
    return json ? json->asObject() : nullptr;
}

Ref<JSON::Object> WebExtensionLocalization::predefinedMessages()
{
    Ref predefinedMessages = JSON::Object::create();

    auto createMessageKey = [](String messageValue) {
        Ref object = JSON::Object::create();
        object->setString(messageKey, messageValue);
        return object;
    };

    if (CheckedPtr locale = m_locale.get(); locale && !m_localeString.isEmpty()) {
        if (locale->defaultWritingDirection() != WebCore::Locale::WritingDirection::RightToLeft) {
            predefinedMessages->setObject(predefinedMessageLanguageDirection, createMessageKey(predefinedMessageValueLeftToRight));
            predefinedMessages->setObject(predefinedMessageLanguageDirectionReversed, createMessageKey(predefinedMessageValueRightToLeft));
            predefinedMessages->setObject(predefinedMessageTextLeadingEdge, createMessageKey(predefinedMessageValueTextEdgeLeft));
            predefinedMessages->setObject(predefinedMessageTextTrailingEdge, createMessageKey(predefinedMessageValueTextEdgeRight));
        } else {
            predefinedMessages->setObject(predefinedMessageLanguageDirection, createMessageKey(predefinedMessageValueRightToLeft));
            predefinedMessages->setObject(predefinedMessageLanguageDirectionReversed, createMessageKey(predefinedMessageValueLeftToRight));
            predefinedMessages->setObject(predefinedMessageTextLeadingEdge, createMessageKey(predefinedMessageValueTextEdgeRight));
            predefinedMessages->setObject(predefinedMessageTextTrailingEdge, createMessageKey(predefinedMessageValueTextEdgeLeft));
        }
    }

    if (!m_localeString.isNull())
        predefinedMessages->setObject(predefinedMessageUILocale, createMessageKey(m_localeString));

    if (!m_uniqueIdentifier.isNull())
        predefinedMessages->setObject(predefinedMessageExtensionID, createMessageKey(m_uniqueIdentifier));

    return predefinedMessages;
}

String WebExtensionLocalization::stringByReplacingNamedPlaceholdersInString(String sourceString, RefPtr<JSON::Object> placeholders)
{
    StringBuilder result;
    for (unsigned index = 0; index < sourceString.length();) {
        if (sourceString[index] != '$') {
            result.append(sourceString[index++]);
            continue;
        }
        if (index + 1 < sourceString.length() && sourceString[index + 1] == '$') {
            // Preserve escaped runs for the positional pass. They must not
            // become named placeholders while scanning this catalog text.
            do {
                result.append('$');
                ++index;
            } while (index < sourceString.length() && sourceString[index] == '$');
            continue;
        }
        unsigned end = index + 1;
        while (end < sourceString.length() && (isASCIIAlphanumeric(sourceString[end]) || sourceString[end] == '_' || sourceString[end] == '@'))
            ++end;
        if (end > index + 1 && end < sourceString.length() && sourceString[end] == '$') {
            auto key = sourceString.substring(index + 1, end - index - 1).convertToASCIILowercase();
            if (RefPtr placeholder = placeholders ? placeholders->getObject(key) : nullptr)
                result.append(placeholder->getString(placeholderDictionaryContentKey));
            index = end + 1;
        } else
            result.append(sourceString[index++]);
    }
    return result.toString();
}

String WebExtensionLocalization::stringByReplacingPositionalPlaceholdersInString(String sourceString, Vector<String> placeholders)
{
    StringBuilder result;
    for (unsigned index = 0; index < sourceString.length();) {
        if (sourceString[index] != '$' || index + 1 == sourceString.length()) {
            result.append(sourceString[index++]);
            continue;
        }
        auto next = sourceString[index + 1];
        if (next == '$') {
            // Consume one escape character per run, before inserting any
            // caller text. Inserted dollar patterns are never scanned again.
            ++index;
            do {
                result.append('$');
                ++index;
            } while (index < sourceString.length() && sourceString[index] == '$');
        } else if (isASCIIDigit(next)) {
            unsigned position = next - '0';
            if (position && position <= placeholders.size())
                result.append(placeholders[position - 1]);
            index += 2;
        } else
            result.append(sourceString[index++]);
    }
    return result.toString();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
