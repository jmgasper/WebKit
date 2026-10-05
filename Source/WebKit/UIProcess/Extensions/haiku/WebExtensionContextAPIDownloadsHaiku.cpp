/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebExtensionDownloadsHaiku.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include "WebPageProxy.h"
#include <Entry.h>
#include <Roster.h>
#include <WebCore/FormData.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/ResourceRequest.h>
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/downloads

bool WebExtensionContext::isDownloadsMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasPermission(WebExtensionPermission::downloads());
}

static std::optional<uint64_t> toDownloadIdentifier(double identifier)
{
    if (!std::isfinite(identifier) || std::trunc(identifier) != identifier || identifier < 1 || identifier > static_cast<double>(1ull << 53))
        return std::nullopt;
    return static_cast<uint64_t>(identifier);
}

static RefPtr<JSON::Object> parseObject(const String& json)
{
    RefPtr value = JSON::Value::parseJSON(json);
    return value ? value->asObject() : nullptr;
}

static std::expected<void, String> openFolder(const String& path)
{
    BEntry entry(path.utf8().legacyCStringPointer(), true);
    entry_ref reference;
    if (entry.InitCheck() != B_OK || !entry.IsDirectory() || entry.GetRef(&reference) != B_OK)
        return makeUnexpected("The folder does not exist"_s);
    // Tracker opens a window for the folder. A running Tracker receives the
    // reference and answers B_ALREADY_RUNNING.
    auto status = be_roster->Launch(&reference);
    if (status != B_OK && status != B_ALREADY_RUNNING)
        return makeUnexpected("The folder could not be opened"_s);
    return { };
}

void WebExtensionContext::downloadsDownload(WebPageProxyIdentifier webPageProxyIdentifier, String&& optionsJSON, CompletionHandler<void(std::expected<double, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.download()"_s;
    auto fail = [&](const String& key, const String& reason) {
        completionHandler(toWebExtensionError(apiName, key, reason));
    };

    RefPtr controller = extensionController();
    RefPtr options = parseObject(optionsJSON);
    if (!controller || !options) {
        fail(nullString(), "the options are invalid"_s);
        return;
    }

    static constexpr ASCIILiteral knownKeys[] { "url"_s, "filename"_s, "conflictAction"_s, "saveAs"_s, "method"_s, "headers"_s, "body"_s, "incognito"_s, "allowHttpErrors"_s };
    for (auto& entry : *options) {
        if (std::ranges::find(knownKeys, entry.key) == std::end(knownKeys)) {
            fail(nullString(), makeString("the property '"_s, entry.key, "' is not supported"_s));
            return;
        }
    }
    auto member = [&](ASCIILiteral key) -> RefPtr<JSON::Value> {
        RefPtr value = options->getValue(key);
        return value && !value->isNull() ? value : nullptr;
    };

    // Relative URLs resolve against the extension, as in other browsers.
    RefPtr urlValue = member("url"_s);
    auto urlString = urlValue ? urlValue->asString() : String();
    URL url { baseURL(), urlString };
    if (urlString.isEmpty() || !url.isValid()) {
        fail("url"_s, "it must be a valid URL"_s);
        return;
    }
    if (url.protocolIsBlob()) {
        // Only the extension's own blobs; they are resolved against the
        // origin of the calling extension page.
        auto ownPrefix = makeString("blob:"_s, baseURL().protocol(), "://"_s, baseURL().host(), '/');
        if (!url.string().startsWith(ownPrefix)) {
            fail("url"_s, "a blob URL must belong to this extension"_s);
            return;
        }
    } else if (!url.protocolIsInHTTPFamily() && !url.protocolIsData()) {
        fail("url"_s, "only http, https, data and this extension's blob URLs can be downloaded"_s);
        return;
    }

    String filename;
    if (RefPtr value = member("filename"_s)) {
        filename = value->asString();
        // Chrome and Firefox reject absolute paths and back-references. The
        // native registry has one flat destination directory, so a relative
        // path contributes its last component only.
        if (filename.isEmpty() || filename.startsWith('/') || filename.contains('\0') || filename.contains('\\')) {
            fail("filename"_s, "it must be a non-empty relative path"_s);
            return;
        }
        for (auto component : StringView(filename).split('/')) {
            if (component == ".."_s || component == "."_s) {
                fail("filename"_s, "it must not contain '.' or '..' components"_s);
                return;
            }
        }
        filename = filename.substring(filename.reverseFind('/') + 1);
        if (filename.isEmpty()) {
            fail("filename"_s, "it must name a file"_s);
            return;
        }
    }

    if (RefPtr value = member("conflictAction"_s)) {
        auto action = value->asString();
        // Existing files are never overwritten and no prompt is shown; every
        // action receives a unique name.
        if (action != "uniquify"_s && action != "overwrite"_s && action != "prompt"_s) {
            fail("conflictAction"_s, "it must be 'uniquify', 'overwrite' or 'prompt'"_s);
            return;
        }
    }
    for (auto key : { "saveAs"_s, "allowHttpErrors"_s }) {
        if (RefPtr value = member(key); value && !value->asBoolean()) {
            fail(key, "it must be a boolean"_s);
            return;
        }
    }
    if (RefPtr value = member("incognito"_s)) {
        auto incognito = value->asBoolean();
        if (!incognito) {
            fail("incognito"_s, "it must be a boolean"_s);
            return;
        }
        if (*incognito && controller->storageIsPersistent()) {
            fail("incognito"_s, "private downloads must be started from a private browsing context"_s);
            return;
        }
    }

    WebCore::ResourceRequest request { URL { url } };
    if (RefPtr value = member("method"_s)) {
        auto method = value->asString();
        if (method != "GET"_s && method != "POST"_s) {
            fail("method"_s, "it must be 'GET' or 'POST'"_s);
            return;
        }
        request.setHTTPMethod(method);
    }
    if (RefPtr value = member("headers"_s)) {
        RefPtr headers = value->asArray();
        if (!headers) {
            fail("headers"_s, "it must be an array of {name, value} objects"_s);
            return;
        }
        for (Ref header : *headers) {
            RefPtr object = header->asObject();
            auto name = object ? object->getString("name"_s) : String();
            auto headerValue = object ? object->getString("value"_s) : String();
            if (name.isNull() || headerValue.isNull() || !WebCore::isValidHTTPToken(name) || !WebCore::isValidHTTPHeaderValue(headerValue)) {
                fail("headers"_s, "it must be an array of valid {name, value} objects"_s);
                return;
            }
            if (WebCore::isForbiddenHeader(name, headerValue)) {
                fail("headers"_s, makeString("the header '"_s, name, "' is not allowed"_s));
                return;
            }
            request.addHTTPHeaderField(name, headerValue);
        }
    }
    if (RefPtr value = member("body"_s)) {
        auto body = value->asString();
        if (body.isNull() || request.httpMethod() != "POST"_s) {
            fail("body"_s, "it must be a string and requires the POST method"_s);
            return;
        }
        request.setHTTPBody(WebCore::FormData::create(body.utf8()));
    }

    // Start the transfer as the calling extension page. That page owns the
    // blob URLs it created; a page of another extension or a web page is refused.
    RefPtr<WebPageProxy> initiatingPage;
    enumerateExtensionPages([&](WebPageProxy& page, bool& stop) {
        if (page.identifier() == webPageProxyIdentifier) {
            initiatingPage = &page;
            stop = true;
        }
    });
    if (!initiatingPage) {
        fail(nullString(), "downloads can be started only from this extension's pages"_s);
        return;
    }

    auto result = controller->nativeDownloads().start(*this, *initiatingPage, WTF::move(request), filename);
    if (!result) {
        fail(nullString(), result.error());
        return;
    }
    completionHandler(static_cast<double>(*result));
}

void WebExtensionContext::downloadsSearch(String&& queryJSON, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.search()"_s;
    RefPtr controller = extensionController();
    RefPtr query = parseObject(queryJSON);
    if (!controller || !query) {
        completionHandler(toWebExtensionError(apiName, "query"_s, "it must be an object"_s));
        return;
    }
    auto result = controller->nativeDownloads().search(*query);
    if (!result) {
        completionHandler(toWebExtensionError(apiName, nullString(), result.error()));
        return;
    }
    completionHandler(result.value()->toJSONString());
}

void WebExtensionContext::downloadsErase(String&& queryJSON, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.erase()"_s;
    RefPtr controller = extensionController();
    RefPtr query = parseObject(queryJSON);
    if (!controller || !query) {
        completionHandler(toWebExtensionError(apiName, "query"_s, "it must be an object"_s));
        return;
    }
    auto result = controller->nativeDownloads().erase(*query);
    if (!result) {
        completionHandler(toWebExtensionError(apiName, nullString(), result.error()));
        return;
    }
    Ref identifiers = JSON::Array::create();
    for (auto identifier : *result)
        identifiers->pushDouble(static_cast<double>(identifier));
    completionHandler(identifiers->toJSONString());
}

void WebExtensionContext::downloadsCancel(double downloadIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.cancel()"_s;
    RefPtr controller = extensionController();
    auto identifier = toDownloadIdentifier(downloadIdentifier);
    if (!controller || !identifier) {
        completionHandler(toWebExtensionError(apiName, "downloadId"_s, "it is not a valid download identifier"_s));
        return;
    }
    auto result = controller->nativeDownloads().cancel(*identifier);
    if (!result) {
        completionHandler(toWebExtensionError(apiName, nullString(), result.error()));
        return;
    }
    completionHandler({ });
}

void WebExtensionContext::downloadsShow(double downloadIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.show()"_s;
    RefPtr controller = extensionController();
    auto identifier = toDownloadIdentifier(downloadIdentifier);
    if (!controller || !identifier) {
        completionHandler(toWebExtensionError(apiName, "downloadId"_s, "it is not a valid download identifier"_s));
        return;
    }
    auto path = controller->nativeDownloads().pathForShow(*identifier);
    if (!path) {
        completionHandler(toWebExtensionError(apiName, nullString(), path.error()));
        return;
    }
    // Tracker shows the containing folder; the file is not selected.
    if (auto result = openFolder(FileSystem::parentPath(*path)); !result) {
        completionHandler(toWebExtensionError(apiName, nullString(), result.error()));
        return;
    }
    completionHandler({ });
}

void WebExtensionContext::downloadsShowDefaultFolder(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "downloads.showDefaultFolder()"_s;
    RefPtr controller = extensionController();
    auto directory = controller ? controller->nativeDownloads().directory() : String();
    if (directory.isEmpty()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the browser has no download directory configured"_s));
        return;
    }
    if (auto result = openFolder(directory); !result) {
        completionHandler(toWebExtensionError(apiName, nullString(), result.error()));
        return;
    }
    completionHandler({ });
}

void WebExtensionContext::fireDownloadsEventIfNeeded(WebExtensionEventListenerType type, const String& argumentJSON)
{
    auto loadIdentifier = privilegedIdentifier();
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, type, argumentJSON, loadIdentifier] {
        if (!isLoaded() || privilegedIdentifier() != loadIdentifier || !hasPermission(WebExtensionPermission::downloads()))
            return;
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchDownloadsEvent(type, argumentJSON));
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
