/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionWebRequestMergeHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <algorithm>
#include <wtf/HashMap.h>

namespace WebKit {
namespace {
using Header = WebExtensionWebRequestHeaderHaiku;
using Reply = WebExtensionWebRequestReplyHaiku;
using Result = WebExtensionWebRequestMergedReplyHaiku;
using Action = WebExtensionWebRequestIgnoredActionHaiku;
using Operation = WebExtensionWebRequestHeaderOperationHaiku;
using HeaderMap = HashMap<String, Header>;

bool sameHeader(const Header& a, const Header& b)
{
    return equalIgnoringASCIICase(a.name, b.name) && a.value == b.value;
}

bool containsHeader(const Vector<Header>& headers, const Header& target)
{
    return std::ranges::any_of(headers, [&](auto& header) { return sameHeader(header, target); });
}

HeaderMap requestHeaderMap(const Vector<Header>& headers, const Reply& reply)
{
    HeaderMap result;
    for (auto& header : headers) {
        auto name = header.name.convertToASCIILowercase();
        if (!reply.hiddenHeaderNames.contains(name))
            result.set(WTF::move(name), header);
    }
    return result;
}

void ignore(Result& result, const Reply& reply, Action action)
{
    result.ignored.append({ reply.installationIdentifier, reply.listenerOrder, action });
}

bool cancelRedirect(const URL& url)
{
    return url.protocolIs("data"_s) || url.string() == "about:blank"_s;
}

void mergeRedirects(Result& result, const URL& originalURL, const Vector<const Reply*>& ordered)
{
    if (originalURL.protocolIs("ws"_s) || originalURL.protocolIs("wss"_s))
        return;
    bool hasCancelRedirect = std::ranges::any_of(ordered, [](auto* reply) {
        return reply->decision.redirectURL && cancelRedirect(*reply->decision.redirectURL);
    });
    for (auto* reply : ordered) {
        auto& destination = reply->decision.redirectURL;
        if (!destination || (hasCancelRedirect && !cancelRedirect(*destination)))
            continue;
        if (!result.redirectURL) {
            result.redirectURL = destination;
            result.redirectingInstallation = reply->installationIdentifier;
        } else if (*result.redirectURL != *destination)
            ignore(result, *reply, Action::Redirect);
    }
}

void mergeRequestHeaders(Result& result, const Vector<Header>& baseline,
    const HashMap<String, Operation>& constraints, const Vector<const Reply*>& ordered)
{
    Vector<Header> merged = baseline;
    HashMap<String, Vector<uint8_t>> set;
    HashSet<String> removed;
    // Rule-engine edits precede imperative webRequest edits. Set and append
    // values are already present in baseline and may not be removed/overwritten.
    for (auto& constraint : constraints) {
        if (constraint.value == Operation::Remove)
            removed.add(constraint.key);
        else {
            for (auto& header : baseline) {
                if (equalIgnoringASCIICase(header.name, constraint.key))
                    set.set(constraint.key, header.value);
            }
        }
    }
    bool changed = false;
    for (auto* reply : ordered) {
        if (!reply->decision.requestHeaders)
            continue;
        auto old = requestHeaderMap(baseline, *reply);
        auto replacement = requestHeaderMap(*reply->decision.requestHeaders, *reply);
        Vector<String> deletions;
        HeaderMap modifications;
        for (auto& header : old) {
            if (!replacement.contains(header.key))
                deletions.append(header.key);
        }
        for (auto& header : replacement) {
            auto before = old.find(header.key);
            if (before == old.end() || before->value.value != header.value.value)
                modifications.set(header.key, header.value);
        }
        bool conflict = std::ranges::any_of(deletions, [&](auto& name) { return set.contains(name); });
        for (auto& header : modifications) {
            auto prior = set.find(header.key);
            if (removed.contains(header.key) || (prior != set.end() && prior->value != header.value.value))
                conflict = true;
        }
        if (conflict) {
            ignore(result, *reply, Action::RequestHeaders);
            continue;
        }
        // Check the entire delta before applying any of it. A conflicting
        // replacement must not leak its otherwise nonconflicting header edits.
        for (auto& name : deletions) {
            merged.removeAllMatching([&](auto& header) { return equalIgnoringASCIICase(header.name, name); });
            removed.add(name);
            changed = true;
        }
        for (auto& header : modifications) {
            merged.removeAllMatching([&](auto& current) { return equalIgnoringASCIICase(current.name, header.key); });
            merged.append(header.value);
            set.set(header.key, header.value.value);
            changed = true;
        }
    }
    if (changed)
        result.headers = WTF::move(merged);
}

void mergeResponseHeaders(Result& result, const Vector<Header>& baseline,
    const HashMap<String, Operation>& constraints, const Vector<const Reply*>& ordered)
{
    Vector<Header> merged = baseline;
    Vector<Header> removed;
    Vector<Header> added;
    bool changed = false;
    for (auto* reply : ordered) {
        if (!reply->decision.responseHeaders)
            continue;
        auto& replacement = *reply->decision.responseHeaders;
        Vector<Header> deletions;
        Vector<Header> additions;
        for (auto& header : baseline) {
            if (!reply->hiddenHeaderNames.contains(header.name.convertToASCIILowercase())
                && !containsHeader(replacement, header) && !containsHeader(deletions, header))
                deletions.append(header);
        }
        for (auto& header : replacement) {
            if (!reply->hiddenHeaderNames.contains(header.name.convertToASCIILowercase())
                && !containsHeader(baseline, header) && !containsHeader(additions, header))
                additions.append(header);
        }
        bool conflict = false;
        for (auto& header : deletions) {
            if (containsHeader(removed, header) || constraints.contains(header.name.convertToASCIILowercase()))
                conflict = true;
        }
        for (auto& header : additions) {
            auto constraint = constraints.find(header.name.convertToASCIILowercase());
            if (constraint != constraints.end() && constraint->value != Operation::Append)
                conflict = true;
        }
        if (conflict) {
            ignore(result, *reply, Action::ResponseHeaders);
            continue;
        }
        for (auto& header : deletions) {
            merged.removeAllMatching([&](auto& current) { return sameHeader(current, header); });
            removed.append(header);
            changed = true;
        }
        for (auto& header : additions) {
            if (!containsHeader(added, header)) {
                merged.append(header);
                added.append(header);
                changed = true;
            }
        }
    }
    if (changed)
        result.headers = WTF::move(merged);
}
}

std::expected<WebExtensionWebRequestMergedReplyHaiku, String> mergeWebExtensionWebRequestRepliesHaiku(
    WebExtensionWebRequestPhaseHaiku phase, const URL& originalURL, const Vector<Header>& originalHeaders,
    const Vector<WebExtensionWebRequestHeaderConstraintHaiku>& headerConstraints, const Vector<Reply>& replies)
{
    constexpr size_t maximumReplies = 256;
    constexpr size_t maximumHeaderBytes = 2 * 1024 * 1024;
    constexpr size_t maximumHeaderEntries = 8192;
    if (static_cast<uint8_t>(phase) > static_cast<uint8_t>(WebExtensionWebRequestPhaseHaiku::AuthRequired))
        return makeUnexpected("The webRequest transaction has an invalid event phase."_s);
    if (replies.size() > maximumReplies)
        return makeUnexpected("Too many pending webRequest replies."_s);
    size_t totalBytes = 0;
    size_t totalEntries = 0;
    auto boundedHeaders = [&](auto& headers) {
        for (auto& header : headers) {
            if (++totalEntries > maximumHeaderEntries || header.name.length() > maximumHeaderBytes - totalBytes)
                return false;
            totalBytes += header.name.length();
            if (header.value.size() > maximumHeaderBytes - totalBytes)
                return false;
            totalBytes += header.value.size();
        }
        return true;
    };
    if (!boundedHeaders(originalHeaders))
        return makeUnexpected("The webRequest header transaction exceeds its bounds."_s);
    Vector<const Reply*> ordered;
    for (auto& reply : replies) {
        if (reply.installationIdentifier.isEmpty() || !reply.installationOrder || !reply.listenerOrder)
            return makeUnexpected("A webRequest dispatch has no immutable installation or listener identity."_s);
        // Compare the bounded retained records directly: all positive uint64
        // priorities, including integer hash-table sentinels, are valid here.
        for (auto* previous : ordered) {
            if ((previous->installationOrder == reply.installationOrder) != (previous->installationIdentifier == reply.installationIdentifier))
                return makeUnexpected("The webRequest installation priorities are ambiguous."_s);
            if (previous->installationOrder == reply.installationOrder && previous->listenerOrder == reply.listenerOrder)
                return makeUnexpected("The webRequest transaction contains a duplicate listener reply."_s);
        }
        if ((reply.decision.requestHeaders && !boundedHeaders(*reply.decision.requestHeaders))
            || (reply.decision.responseHeaders && !boundedHeaders(*reply.decision.responseHeaders)))
            return makeUnexpected("The webRequest header transaction exceeds its bounds."_s);
        ordered.append(&reply);
    }
    std::ranges::sort(ordered, [](auto* a, auto* b) {
        if (a->installationOrder != b->installationOrder)
            return a->installationOrder > b->installationOrder;
        return a->listenerOrder < b->listenerOrder;
    });
    HashMap<String, Operation> constraints;
    for (auto& constraint : headerConstraints) {
        auto entry = constraints.add(constraint.name.convertToASCIILowercase(), constraint.operation);
        if (!entry.isNewEntry && entry.iterator->value != constraint.operation)
            return makeUnexpected("The webRequest transaction has contradictory header constraints."_s);
    }
    Result result;
    for (auto* reply : ordered) {
        if (reply->decision.cancel) {
            result.cancel = true;
            result.cancelingInstallation = reply->installationIdentifier;
            return result;
        }
    }
    switch (phase) {
    case WebExtensionWebRequestPhaseHaiku::BeforeRequest:
        mergeRedirects(result, originalURL, ordered);
        break;
    case WebExtensionWebRequestPhaseHaiku::BeforeSendHeaders:
        mergeRequestHeaders(result, originalHeaders, constraints, ordered);
        break;
    case WebExtensionWebRequestPhaseHaiku::HeadersReceived:
        mergeRedirects(result, originalURL, ordered);
        mergeResponseHeaders(result, originalHeaders, constraints, ordered);
        break;
    case WebExtensionWebRequestPhaseHaiku::AuthRequired:
        for (auto* reply : ordered) {
            if (!reply->decision.authCredentials)
                continue;
            auto& credentials = *reply->decision.authCredentials;
            if (!result.authCredentials)
                result.authCredentials = credentials;
            else if (result.authCredentials->username != credentials.username || result.authCredentials->password != credentials.password)
                ignore(result, *reply, Action::Credentials);
        }
        break;
    default:
        return makeUnexpected("The webRequest transaction has an invalid event phase."_s);
    }
    return result;
}
}
#endif
