/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionWebRequestFilter.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "WebExtensionFrameIdentifier.h"
#include <wtf/text/MakeString.h>

namespace WebKit {

String webExtensionWebRequestResourceType(const ResourceLoadInfo& resource)
{
    switch (resource.type) {
    case ResourceLoadInfo::Type::ApplicationManifest: return "web_manifest"_s;
    case ResourceLoadInfo::Type::Beacon: return "beacon"_s;
    case ResourceLoadInfo::Type::CSPReport: return "csp_report"_s;
    case ResourceLoadInfo::Type::Document: return resource.parentFrameID ? "sub_frame"_s : "main_frame"_s;
    case ResourceLoadInfo::Type::Font: return "font"_s;
    case ResourceLoadInfo::Type::Image: return "image"_s;
    case ResourceLoadInfo::Type::Media: return "media"_s;
    case ResourceLoadInfo::Type::Object: return "object"_s;
    case ResourceLoadInfo::Type::Ping: return "ping"_s;
    case ResourceLoadInfo::Type::Script: return "script"_s;
    case ResourceLoadInfo::Type::Stylesheet: return "stylesheet"_s;
    case ResourceLoadInfo::Type::Fetch:
    case ResourceLoadInfo::Type::XMLHTTPRequest: return "xmlhttprequest"_s;
    case ResourceLoadInfo::Type::XSLT: return "xslt"_s;
    case ResourceLoadInfo::Type::Other: return "other"_s;
    }
    ASSERT_NOT_REACHED();
    return "other"_s;
}

Ref<JSON::Object> webExtensionWebRequestDetails(const ResourceLoadInfo& resource, WebExtensionTabIdentifier tab, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier)
{
    auto result = JSON::Object::create();
    // ResourceLoadInfo retains actual Core identifiers. Normalize only the
    // known main frame to the WebExtension sentinel, including a child's parent.
    // The UI process snapshots main-frame identity before any background wake.
    auto normalizedFrame = [&](std::optional<WebCore::FrameIdentifier> identifier) {
        if (!identifier)
            return WebExtensionFrameConstants::NoneIdentifier;
        if (identifier == mainFrameIdentifier)
            return WebExtensionFrameConstants::MainFrameIdentifier;
        return toWebExtensionFrameIdentifier(identifier);
    };
    auto frame = resource.frameID && !resource.parentFrameID
        ? WebExtensionFrameConstants::MainFrameIdentifier : normalizedFrame(resource.frameID);
    auto parent = normalizedFrame(resource.parentFrameID);
    result->setDouble("frameId"_s, toWebAPI(frame));
    result->setDouble("parentFrameId"_s, toWebAPI(parent));
    result->setString("requestId"_s, makeString(resource.resourceLoadID.toUInt64()));
    result->setDouble("timeStamp"_s, floor(resource.eventTimestamp.secondsSinceEpoch().milliseconds()));
    result->setString("url"_s, resource.originalURL.string());
    result->setDouble("tabId"_s, toWebAPI(tab));
    result->setString("type"_s, webExtensionWebRequestResourceType(resource));
    result->setString("method"_s, resource.originalHTTPMethod);
    if (resource.documentID)
        result->setString("documentId"_s, resource.documentID->toString());
    return result;
}

String webExtensionWebRequestStatusLine(const String& httpVersion, int statusCode, const String& statusText)
{
    // Non-HTTP and incomplete responses need not have protocol/status metadata.
    // Do not invent an HTTP version or a successful response for them.
    if (httpVersion.isEmpty() || statusCode <= 0)
        return emptyString();
    if (statusText.isEmpty())
        return makeString(httpVersion, ' ', statusCode);
    return makeString(httpVersion, ' ', statusCode, ' ', statusText);
}

std::expected<WebExtensionWebRequestFilter, String> WebExtensionWebRequestFilter::parse(const JSON::Value& value)
{
    RefPtr object = value.asObject();
    if (!object)
        return makeUnexpected("The webRequest filter must be an object."_s);
    for (auto& entry : *object) {
        if (entry.key != "urls"_s && entry.key != "types"_s && entry.key != "tabId"_s && entry.key != "windowId"_s)
            return makeUnexpected(makeString("Unsupported webRequest filter field: "_s, entry.key));
    }
    RefPtr urls = object->getArray("urls"_s);
    if (!urls)
        return makeUnexpected("The webRequest filter requires an array of URL patterns."_s);
    WebExtensionWebRequestFilter result;
    for (auto& value : *urls) {
        auto patternString = value->asString();
        if (patternString.isEmpty())
            return makeUnexpected("Every webRequest URL pattern must be a nonempty string."_s);
        // Chrome and Firefox accept WebSocket patterns (Privacy Badger lists
        // ws://*/* beside http://*/*). This port's patterns have no ws or wss
        // scheme, so they can match nothing here: leave them out rather than
        // failing the listener, which aborted the extension's startup.
        if (patternString.startsWith("ws://"_s) || patternString.startsWith("wss://"_s))
            continue;
        RefPtr pattern = WebExtensionMatchPattern::getOrCreate(patternString);
        if (!pattern || !pattern->isSupported())
            return makeUnexpected(makeString("Invalid webRequest URL pattern: "_s, patternString));
        result.m_urlPatterns.append(pattern.releaseNonNull());
    }
    if (RefPtr types = object->getValue("types"_s)) {
        RefPtr array = types->asArray();
        if (!array)
            return makeUnexpected("The webRequest resource types must be an array of strings."_s);
        result.m_types.emplace();
        for (auto& value : *array) {
            auto name = value->asString();
            if (name != "main_frame"_s && name != "sub_frame"_s && name != "stylesheet"_s
                && name != "script"_s && name != "image"_s && name != "font"_s && name != "object"_s
                && name != "xmlhttprequest"_s && name != "ping"_s && name != "csp_report"_s
                && name != "media"_s && name != "websocket"_s && name != "web_manifest"_s
                && name != "xslt"_s && name != "beacon"_s && name != "other"_s)
                return makeUnexpected("Unknown webRequest resource type."_s);
            result.m_types->addVoid(name);
        }
    }
    if (RefPtr tab = object->getValue("tabId"_s)) {
        auto number = tab->asDouble();
        result.m_tab = number ? toWebExtensionTabIdentifier(*number) : std::nullopt;
        if (!result.m_tab)
            return makeUnexpected("The webRequest tabId must be a tab identifier or -1."_s);
    }
    if (RefPtr window = object->getValue("windowId"_s)) {
        auto number = window->asDouble();
        result.m_window = number ? toWebExtensionWindowIdentifier(*number) : std::nullopt;
        if (!result.m_window || isCurrent(result.m_window))
            return makeUnexpected("The webRequest windowId must be a window identifier or -1."_s);
    }
    return result;
}

bool WebExtensionWebRequestFilter::matches(const ResourceLoadInfo& resource, WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window) const
{
    if (m_tab && *m_tab != tab)
        return false;
    if (m_window && *m_window != window)
        return false;
    if (m_types && !m_types->contains(webExtensionWebRequestResourceType(resource)))
        return false;
    return m_urlPatterns.containsIf([&](auto& pattern) { return pattern->matchesURL(resource.originalURL); });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
