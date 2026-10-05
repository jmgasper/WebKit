/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionDownloadsHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIPageConfiguration.h"
#include "DownloadProxy.h"
#include "DownloadRegistryHaiku.h"
#include "WebExtension.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionPermission.h"
#include "WebKitContext.h"
#include "WebPageProxy.h"
#include "WebProcessPool.h"
#include "WebsiteDataStore.h"
#include <JavaScriptCore/RegularExpression.h>
#include <WebCore/ResourceRequest.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sys/stat.h>
#include <tuple>
#include <wtf/DateMath.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebExtensionDownloadsHaiku);

// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/downloads
static constexpr auto stateInProgress = "in_progress"_s;
static constexpr auto stateInterrupted = "interrupted"_s;
static constexpr auto stateComplete = "complete"_s;
static constexpr size_t defaultSearchLimit = 1000;

WebExtensionDownloadsHaiku& WebExtensionController::nativeDownloads()
{
    if (!m_nativeDownloads)
        m_nativeDownloads = makeUnique<WebExtensionDownloadsHaiku>(*this);
    return *m_nativeDownloads;
}

static String isoTime(WallTime time)
{
    double milliseconds = std::floor(time.secondsSinceEpoch().milliseconds());
    time_t seconds = static_cast<time_t>(std::floor(milliseconds / 1000));
    struct tm parts { };
    if (!gmtime_r(&seconds, &parts))
        return emptyString();
    char buffer[32];
    if (!strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts))
        return emptyString();
    auto remainder = static_cast<unsigned>(milliseconds - static_cast<double>(seconds) * 1000);
    return makeString(String::fromLatin1(buffer), '.', pad('0', 3, remainder), 'Z');
}

static bool fileExists(const String& path)
{
    struct stat info;
    return !path.isEmpty() && !stat(path.utf8().legacyCStringPointer(), &info) && S_ISREG(info.st_mode);
}

WebExtensionDownloadsHaiku::WebExtensionDownloadsHaiku(WebExtensionController& controller)
    : m_controller(controller)
{
}

WebExtensionDownloadsHaiku::~WebExtensionDownloadsHaiku()
{
    shutdown();
}

void WebExtensionDownloadsHaiku::attach(const std::shared_ptr<DownloadRegistryHaiku>& registry, bool incognito)
{
    ASSERT(RunLoop::isMain());
    shutdown();
    m_registry = registry;
    m_incognito = incognito;
    if (!registry)
        return;
    // The controller owns this object and clears the observer before destroying it.
    registry->setObserver([weakController = m_controller](const DownloadRegistryEventHaiku& event) {
        if (RefPtr controller = weakController.get())
            controller->nativeDownloads().observe(event);
    });
}

void WebExtensionDownloadsHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    if (auto registry = std::exchange(m_registry, { }).lock())
        registry->setObserver(nullptr);
}

String WebExtensionDownloadsHaiku::directory() const
{
    auto registry = m_registry.lock();
    return registry ? registry->directory() : String();
}

std::expected<uint64_t, String> WebExtensionDownloadsHaiku::start(WebExtensionContext& context, WebPageProxy& page, WebCore::ResourceRequest&& request, const String& filename)
{
    ASSERT(RunLoop::isMain());
    auto registry = m_registry.lock();
    if (!registry)
        return makeUnexpected("Downloads are not available in this browser context"_s);
    if (registry->directory().isEmpty())
        return makeUnexpected("The browser has no download directory configured"_s);

    // The initiating extension page supplies the session, first party and top
    // origin. The network process resolves a blob: URL against that origin, so
    // a blob created by the calling page remains readable by the transfer.
    Ref pool = page.configuration().processPool();
    Ref download = pool->download(page.websiteDataStore(), &page, request, std::nullopt, filename);
    auto identifier = download->downloadID().toUInt64();
    Ref extension = context.extension();
    m_initiators.set(identifier, Initiator { context.uniqueIdentifier(), extension->displayName() });
    // Registration notifies Started synchronously, which records the item and
    // fires onCreated before the caller's promise resolves.
    registry->registerDownload(download.get(), { }, filename);
    m_initiators.remove(identifier);
    if (!m_items.contains(identifier))
        return makeUnexpected("The download could not be started"_s);
    return identifier;
}

Ref<JSON::Object> WebExtensionDownloadsHaiku::toJSON(const Item& item) const
{
    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/downloads#type-DownloadItem
    Ref object = JSON::Object::create();
    object->setDouble("id"_s, static_cast<double>(item.identifier));
    object->setString("url"_s, item.url);
    object->setString("finalUrl"_s, item.finalURL);
    object->setString("referrer"_s, emptyString());
    object->setString("filename"_s, item.filename);
    object->setBoolean("incognito"_s, m_incognito);
    object->setString("danger"_s, "safe"_s);
    object->setString("mime"_s, item.mimeType);
    object->setString("startTime"_s, isoTime(item.startTime));
    if (item.endTime)
        object->setString("endTime"_s, isoTime(*item.endTime));
    object->setString("state"_s, item.state);
    object->setBoolean("paused"_s, false);
    object->setBoolean("canResume"_s, false);
    if (!item.error.isEmpty())
        object->setString("error"_s, item.error);
    object->setDouble("bytesReceived"_s, static_cast<double>(item.bytesReceived));
    object->setDouble("totalBytes"_s, static_cast<double>(item.totalBytes));
    object->setDouble("fileSize"_s, static_cast<double>(item.fileSize));
    object->setBoolean("exists"_s, item.exists);
    if (!item.byExtensionIdentifier.isEmpty()) {
        object->setString("byExtensionId"_s, item.byExtensionIdentifier);
        object->setString("byExtensionName"_s, item.byExtensionName);
    }
    return object;
}

static void addStringDelta(JSON::Object& delta, ASCIILiteral key, const String& previous, const String& current)
{
    if (previous == current)
        return;
    Ref change = JSON::Object::create();
    if (!previous.isEmpty())
        change->setString("previous"_s, previous);
    change->setString("current"_s, current);
    delta.setObject(key, WTF::move(change));
}

static void addNumberDelta(JSON::Object& delta, ASCIILiteral key, int64_t previous, int64_t current)
{
    if (previous == current)
        return;
    Ref change = JSON::Object::create();
    change->setDouble("previous"_s, static_cast<double>(previous));
    change->setDouble("current"_s, static_cast<double>(current));
    delta.setObject(key, WTF::move(change));
}

void WebExtensionDownloadsHaiku::observe(const DownloadRegistryEventHaiku& event)
{
    ASSERT(RunLoop::isMain());
    auto identifier = event.identifier;
    if (!identifier)
        return;

    if (event.type == DownloadRegistryEventHaiku::Type::Started) {
        if (m_items.contains(identifier))
            return;
        Item item;
        item.identifier = identifier;
        item.order = ++m_nextOrder;
        item.url = event.url;
        item.finalURL = event.finalURL;
        item.filename = event.path;
        item.mimeType = event.mimeType;
        item.startTime = WallTime::now();
        item.state = stateInProgress;
        item.bytesReceived = event.currentSize;
        item.totalBytes = event.expectedSize;
        if (auto initiator = m_initiators.getOptional(identifier)) {
            item.byExtensionIdentifier = initiator->identifier;
            item.byExtensionName = initiator->name;
        }
        auto json = toJSON(item)->toJSONString();
        m_items.set(identifier, WTF::move(item));
        dispatch(WebExtensionEventListenerType::DownloadsOnCreated, json);
        return;
    }

    auto found = m_items.find(identifier);
    // Erased items stay erased; late notifications for them are ignored.
    if (found == m_items.end())
        return;
    auto& item = found->value;
    auto previous = item;

    item.bytesReceived = event.currentSize;
    if (!event.path.isEmpty())
        item.filename = event.path;
    if (!event.finalURL.isEmpty())
        item.finalURL = event.finalURL;
    if (!event.mimeType.isEmpty())
        item.mimeType = event.mimeType;
    if (event.expectedSize >= 0)
        item.totalBytes = event.expectedSize;

    if (event.type == DownloadRegistryEventHaiku::Type::Finished) {
        item.endTime = WallTime::now();
        if (event.result == B_WEBKIT_DOWNLOAD_SUCCEEDED) {
            item.state = stateComplete;
            item.fileSize = static_cast<int64_t>(event.currentSize);
            if (item.totalBytes < 0)
                item.totalBytes = item.fileSize;
            item.exists = fileExists(item.filename);
        } else {
            item.state = stateInterrupted;
            // The registry removes partial files on failure and cancellation.
            item.error = event.result == B_WEBKIT_DOWNLOAD_CANCELLED ? "USER_CANCELED"_s
                : item.filename.isEmpty() && event.error == B_IO_ERROR ? "FILE_FAILED"_s : "NETWORK_FAILED"_s;
            item.exists = fileExists(item.filename);
        }
    } else if (!item.filename.isEmpty() && previous.filename.isEmpty())
        item.exists = true;

    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/downloads#type-DownloadDelta
    Ref delta = JSON::Object::create();
    addStringDelta(delta, "url"_s, previous.url, item.url);
    addStringDelta(delta, "finalUrl"_s, previous.finalURL, item.finalURL);
    addStringDelta(delta, "filename"_s, previous.filename, item.filename);
    addStringDelta(delta, "mime"_s, previous.mimeType, item.mimeType);
    addStringDelta(delta, "state"_s, previous.state, item.state);
    addStringDelta(delta, "error"_s, previous.error, item.error);
    if (item.endTime && !previous.endTime)
        addStringDelta(delta, "endTime"_s, { }, isoTime(*item.endTime));
    addNumberDelta(delta, "totalBytes"_s, previous.totalBytes, item.totalBytes);
    addNumberDelta(delta, "fileSize"_s, previous.fileSize, item.fileSize);
    if (previous.exists != item.exists) {
        Ref change = JSON::Object::create();
        change->setBoolean("previous"_s, previous.exists);
        change->setBoolean("current"_s, item.exists);
        delta->setObject("exists"_s, WTF::move(change));
    }
    // bytesReceived is not part of a DownloadDelta; progress alone is silent.
    if (!delta->size())
        return;
    delta->setDouble("id"_s, static_cast<double>(identifier));
    dispatch(WebExtensionEventListenerType::DownloadsOnChanged, delta->toJSONString());
}

void WebExtensionDownloadsHaiku::dispatch(WebExtensionEventListenerType type, const String& argumentJSON)
{
    RefPtr controller = m_controller.get();
    if (!controller)
        return;
    auto contexts = controller->extensionContexts();
    for (Ref context : contexts) {
        if (!context->isLoaded() || !context->hasPermission(WebExtensionPermission::downloads()))
            continue;
        if (m_incognito && !context->hasAccessToPrivateData())
            continue;
        context->fireDownloadsEventIfNeeded(type, argumentJSON);
    }
}

static std::optional<double> timeFilterValue(const JSON::Value& value)
{
    if (auto number = value.asDouble())
        return std::isfinite(*number) ? std::optional { *number } : std::nullopt;
    auto string = value.asString();
    if (string.isEmpty() || !string.containsOnlyASCII())
        return std::nullopt;
    auto latin1 = string.latin1();
    auto milliseconds = WTF::parseDate(byteCast<Latin1Character>(latin1.span()));
    return std::isfinite(milliseconds) ? std::optional { milliseconds } : std::nullopt;
}

auto WebExtensionDownloadsHaiku::query(const JSON::Object& query) const -> std::expected<Vector<const Item*>, String>
{
    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/downloads#type-DownloadQuery
    static constexpr ASCIILiteral known[] {
        "query"_s, "id"_s, "url"_s, "urlRegex"_s, "filename"_s, "filenameRegex"_s, "mime"_s, "state"_s, "danger"_s,
        "paused"_s, "error"_s, "exists"_s, "bytesReceived"_s, "totalBytes"_s, "fileSize"_s, "totalBytesGreater"_s,
        "totalBytesLess"_s, "startTime"_s, "endTime"_s, "startedBefore"_s, "startedAfter"_s, "endedBefore"_s,
        "endedAfter"_s, "limit"_s, "orderBy"_s, "finalUrl"_s, "finalUrlRegex"_s, "incognito"_s
    };
    for (auto& entry : query) {
        if (std::ranges::find(known, entry.key) == std::end(known))
            return makeUnexpected(makeString("Unsupported download query property: "_s, entry.key));
    }

    auto value = [&](ASCIILiteral key) -> RefPtr<JSON::Value> {
        RefPtr result = query.getValue(key);
        return result && !result->isNull() ? result : nullptr;
    };
    auto typeError = [](ASCIILiteral key, ASCIILiteral type) {
        return makeUnexpected(makeString("The '"_s, key, "' value is invalid, because it must be "_s, type));
    };

    Vector<std::pair<String, bool>> terms;
    if (RefPtr terms_ = value("query"_s)) {
        RefPtr array = terms_->asArray();
        if (!array)
            return typeError("query"_s, "an array of strings"_s);
        for (Ref term : *array) {
            auto string = term->asString();
            if (string.isNull())
                return typeError("query"_s, "an array of strings"_s);
            bool negative = string.startsWith('-');
            auto text = negative ? string.substring(1) : string;
            if (!text.isEmpty())
                terms.append({ text.convertToASCIILowercase(), negative });
        }
    }

    struct StringFilter { ASCIILiteral key; String Item::* member; };
    Vector<std::pair<String Item::*, String>> exactStrings;
    for (auto filter : std::initializer_list<StringFilter> { { "url"_s, &Item::url }, { "finalUrl"_s, &Item::finalURL }, { "filename"_s, &Item::filename },
        { "mime"_s, &Item::mimeType }, { "state"_s, &Item::state }, { "error"_s, &Item::error } }) {
        if (RefPtr filterValue = value(filter.key)) {
            auto string = filterValue->asString();
            if (string.isNull())
                return typeError(filter.key, "a string"_s);
            exactStrings.append({ filter.member, string });
        }
    }

    struct RegexFilter { ASCIILiteral key; String Item::* member; };
    Vector<std::pair<String Item::*, JSC::Yarr::RegularExpression>> expressions;
    for (auto filter : std::initializer_list<RegexFilter> { { "urlRegex"_s, &Item::url }, { "finalUrlRegex"_s, &Item::finalURL }, { "filenameRegex"_s, &Item::filename } }) {
        if (RefPtr filterValue = value(filter.key)) {
            auto string = filterValue->asString();
            if (string.isNull())
                return typeError(filter.key, "a string"_s);
            JSC::Yarr::RegularExpression expression(string);
            if (!expression.isValid())
                return makeUnexpected(makeString("The '"_s, filter.key, "' value is not a valid regular expression"_s));
            expressions.append({ filter.member, WTF::move(expression) });
        }
    }

    std::optional<double> identifier;
    if (RefPtr filterValue = value("id"_s)) {
        identifier = filterValue->asDouble();
        if (!identifier)
            return typeError("id"_s, "a number"_s);
    }

    std::optional<bool> exists, paused, incognito;
    for (auto [key, target] : std::initializer_list<std::pair<ASCIILiteral, std::optional<bool>*>> { { "exists"_s, &exists }, { "paused"_s, &paused }, { "incognito"_s, &incognito } }) {
        if (RefPtr filterValue = value(key)) {
            *target = filterValue->asBoolean();
            if (!*target)
                return typeError(key, "a boolean"_s);
        }
    }
    String danger;
    if (RefPtr filterValue = value("danger"_s)) {
        danger = filterValue->asString();
        if (danger.isNull())
            return typeError("danger"_s, "a string"_s);
    }

    enum class NumberField : uint8_t { BytesReceived, TotalBytes, FileSize };
    enum class Comparison : uint8_t { Equal, Greater, Less };
    struct NumberFilter { NumberField field; Comparison comparison; double value; };
    Vector<NumberFilter> numbers;
    for (auto [key, field, comparison] : std::initializer_list<std::tuple<ASCIILiteral, NumberField, Comparison>> {
        { "bytesReceived"_s, NumberField::BytesReceived, Comparison::Equal }, { "totalBytes"_s, NumberField::TotalBytes, Comparison::Equal },
        { "fileSize"_s, NumberField::FileSize, Comparison::Equal }, { "totalBytesGreater"_s, NumberField::TotalBytes, Comparison::Greater },
        { "totalBytesLess"_s, NumberField::TotalBytes, Comparison::Less } }) {
        if (RefPtr filterValue = value(key)) {
            auto number = filterValue->asDouble();
            if (!number)
                return typeError(key, "a number"_s);
            numbers.append({ field, comparison, *number });
        }
    }

    enum class TimeField : uint8_t { Start, End };
    struct TimeFilter { TimeField field; Comparison comparison; double milliseconds; };
    Vector<TimeFilter> times;
    for (auto [key, field, comparison] : std::initializer_list<std::tuple<ASCIILiteral, TimeField, Comparison>> {
        { "startTime"_s, TimeField::Start, Comparison::Equal }, { "endTime"_s, TimeField::End, Comparison::Equal },
        { "startedBefore"_s, TimeField::Start, Comparison::Less }, { "startedAfter"_s, TimeField::Start, Comparison::Greater },
        { "endedBefore"_s, TimeField::End, Comparison::Less }, { "endedAfter"_s, TimeField::End, Comparison::Greater } }) {
        if (RefPtr filterValue = value(key)) {
            auto milliseconds = timeFilterValue(*filterValue);
            if (!milliseconds)
                return typeError(key, "a date string or a number of milliseconds"_s);
            times.append({ field, comparison, *milliseconds });
        }
    }

    size_t limit = defaultSearchLimit;
    if (RefPtr filterValue = value("limit"_s)) {
        auto number = filterValue->asDouble();
        if (!number || !std::isfinite(*number) || *number < 0 || std::trunc(*number) != *number)
            return typeError("limit"_s, "a non-negative integer"_s);
        limit = *number ? static_cast<size_t>(std::min(*number, 1e9)) : std::numeric_limits<size_t>::max();
    }

    Vector<std::pair<String, bool>> orderBy;
    if (RefPtr filterValue = value("orderBy"_s)) {
        RefPtr array = filterValue->asArray();
        if (!array)
            return typeError("orderBy"_s, "an array of strings"_s);
        for (Ref key : *array) {
            auto string = key->asString();
            if (string.isEmpty())
                return typeError("orderBy"_s, "an array of strings"_s);
            bool descending = string.startsWith('-');
            auto name = descending ? string.substring(1) : string;
            if (name != "startTime"_s && name != "endTime"_s && name != "id"_s && name != "url"_s && name != "filename"_s
                && name != "bytesReceived"_s && name != "totalBytes"_s && name != "fileSize"_s && name != "state"_s && name != "mime"_s)
                return makeUnexpected(makeString("Unsupported download orderBy property: "_s, name));
            orderBy.append({ name, descending });
        }
    }

    Vector<const Item*> matches;
    for (auto& entry : m_items) {
        auto& item = entry.value;
        if (identifier && static_cast<double>(item.identifier) != *identifier)
            continue;
        if (!std::ranges::all_of(exactStrings, [&](auto& filter) { return item.*(filter.first) == filter.second; }))
            continue;
        if (!std::ranges::all_of(expressions, [&](auto& filter) { return filter.second.match(item.*(filter.first)) >= 0; }))
            continue;
        if (exists && item.exists != *exists)
            continue;
        if ((paused && *paused) || (incognito && *incognito != m_incognito) || (!danger.isNull() && danger != "safe"_s))
            continue;
        auto numberMatches = [&](const NumberFilter& filter) {
            double actual = filter.field == NumberField::BytesReceived ? static_cast<double>(item.bytesReceived)
                : filter.field == NumberField::TotalBytes ? static_cast<double>(item.totalBytes) : static_cast<double>(item.fileSize);
            return filter.comparison == Comparison::Equal ? actual == filter.value
                : filter.comparison == Comparison::Greater ? actual > filter.value : actual < filter.value;
        };
        if (!std::ranges::all_of(numbers, numberMatches))
            continue;
        auto timeMatches = [&](const TimeFilter& filter) {
            if (filter.field == TimeField::End && !item.endTime)
                return false;
            double actual = std::floor((filter.field == TimeField::Start ? item.startTime : *item.endTime).secondsSinceEpoch().milliseconds());
            return filter.comparison == Comparison::Equal ? actual == std::floor(filter.milliseconds)
                : filter.comparison == Comparison::Greater ? actual > filter.milliseconds : actual < filter.milliseconds;
        };
        if (!std::ranges::all_of(times, timeMatches))
            continue;
        if (!terms.isEmpty()) {
            auto haystack = makeString(item.url, '\n', item.filename).convertToASCIILowercase();
            if (!std::ranges::all_of(terms, [&](auto& term) { return haystack.contains(term.first) != term.second; }))
                continue;
        }
        matches.append(&item);
    }

    // Registration order is the default and final tie-breaker.
    std::ranges::sort(matches, [&](const Item* a, const Item* b) {
        for (auto& [name, descending] : orderBy) {
            int comparison = 0;
            auto compareNumbers = [](double x, double y) { return x < y ? -1 : x > y ? 1 : 0; };
            if (name == "startTime"_s)
                comparison = compareNumbers(a->startTime.secondsSinceEpoch().value(), b->startTime.secondsSinceEpoch().value());
            else if (name == "endTime"_s)
                comparison = compareNumbers(a->endTime ? a->endTime->secondsSinceEpoch().value() : -1, b->endTime ? b->endTime->secondsSinceEpoch().value() : -1);
            else if (name == "id"_s)
                comparison = compareNumbers(a->identifier, b->identifier);
            else if (name == "bytesReceived"_s)
                comparison = compareNumbers(a->bytesReceived, b->bytesReceived);
            else if (name == "totalBytes"_s)
                comparison = compareNumbers(a->totalBytes, b->totalBytes);
            else if (name == "fileSize"_s)
                comparison = compareNumbers(a->fileSize, b->fileSize);
            else {
                auto& x = name == "url"_s ? a->url : name == "filename"_s ? a->filename : name == "state"_s ? a->state : a->mimeType;
                auto& y = name == "url"_s ? b->url : name == "filename"_s ? b->filename : name == "state"_s ? b->state : b->mimeType;
                auto order = codePointCompare(x, y);
                comparison = is_lt(order) ? -1 : is_gt(order) ? 1 : 0;
            }
            if (comparison)
                return descending ? comparison > 0 : comparison < 0;
        }
        return a->order < b->order;
    });
    if (matches.size() > limit)
        matches.shrink(limit);
    return matches;
}

std::expected<Ref<JSON::Array>, String> WebExtensionDownloadsHaiku::search(const JSON::Object& queryObject) const
{
    auto matches = query(queryObject);
    if (!matches)
        return makeUnexpected(matches.error());
    Ref result = JSON::Array::create();
    for (auto* item : *matches)
        result->pushObject(toJSON(*item));
    return result;
}

std::expected<Vector<uint64_t>, String> WebExtensionDownloadsHaiku::erase(const JSON::Object& queryObject)
{
    auto matches = query(queryObject);
    if (!matches)
        return makeUnexpected(matches.error());
    Vector<uint64_t> identifiers;
    for (auto* item : *matches)
        identifiers.append(item->identifier);
    // Erasing removes history only. An active transfer keeps running; its
    // later notifications no longer reach extensions.
    for (auto identifier : identifiers) {
        m_items.remove(identifier);
        dispatch(WebExtensionEventListenerType::DownloadsOnErased, String::number(identifier));
    }
    return identifiers;
}

std::expected<void, String> WebExtensionDownloadsHaiku::cancel(uint64_t identifier)
{
    auto found = m_items.find(identifier);
    if (found == m_items.end())
        return makeUnexpected(makeString("No download with the identifier "_s, identifier, " exists"_s));
    // Cancelling a finished download is not an error.
    if (found->value.state != stateInProgress)
        return { };
    auto registry = m_registry.lock();
    if (registry && registry->isActive(identifier))
        registry->cancel(identifier);
    return { };
}

std::expected<String, String> WebExtensionDownloadsHaiku::pathForShow(uint64_t identifier) const
{
    auto found = m_items.find(identifier);
    if (found == m_items.end())
        return makeUnexpected(makeString("No download with the identifier "_s, identifier, " exists"_s));
    if (found->value.filename.isEmpty())
        return makeUnexpected("The download has no file yet"_s);
    return found->value.filename;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
