/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionTabScriptTarget.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "FrameTreeNodeData.h"

namespace WebKit {

static bool validFrame(const FrameInfoData& frame, WebPageProxyIdentifier pageIdentifier, const FrameInfoData* parent)
{
    return frame.webPageProxyID && *frame.webPageProxyID == pageIdentifier
        && frame.documentID && *frame.documentID
        && frame.isMainFrame == !parent
        && (parent ? frame.parentFrameID && *frame.parentFrameID == parent->frameID : !frame.parentFrameID);
}

std::expected<Vector<WebExtensionTabScriptTarget>, String> captureWebExtensionTabScriptTargets(const FrameTreeNodeData& root, WebPageProxyIdentifier pageIdentifier, const WebExtensionTabScriptParameters& parameters)
{
    if ((parameters.allFrames && (parameters.frameIdentifier || parameters.documentIdentifier))
        || (parameters.frameIdentifier && parameters.documentIdentifier))
        return makeUnexpected("conflicting script targets"_s);

    struct PendingFrame {
        const FrameTreeNodeData* node;
        Vector<const FrameInfoData*> ancestors;
    };
    Vector<PendingFrame> pending;
    pending.append({ &root, { } });
    Vector<WebExtensionTabScriptTarget> targets;
    while (!pending.isEmpty()) {
        auto current = pending.takeLast();
        const auto& info = current.node->info;
        bool matches = parameters.allFrames;
        if (parameters.documentIdentifier)
            matches = info.documentID && info.documentID->object() == *parameters.documentIdentifier;
        else if (!parameters.allFrames) {
            auto identifier = parameters.frameIdentifier.value_or(WebExtensionFrameConstants::MainFrameIdentifier);
            matches = isMainFrame(identifier) ? info.isMainFrame : !info.isMainFrame && info.frameID.toUInt64() == identifier.toUInt64();
        }

        current.ancestors.append(&info);
        if (matches) {
            WebExtensionTabScriptTarget target { pageIdentifier, { } };
            const FrameInfoData* parent = nullptr;
            for (const auto* frame : current.ancestors) {
                if (!validFrame(*frame, pageIdentifier, parent))
                    return makeUnexpected("the target document is unavailable"_s);
                target.documents.append({ frame->frameID, *frame->documentID, frame->request.url() });
                parent = frame;
            }
            targets.append(WTF::move(target));
        }

        // Reverse insertion preserves tree order with an iterative traversal.
        // A deeply nested frame tree must not consume the native call stack.
        const auto& children = current.node->children;
        for (size_t i = children.size(); i; --i)
            pending.append({ &children[i - 1], current.ancestors });
    }
    if (targets.isEmpty())
        return makeUnexpected("the requested frame or document was not found"_s);
    if (!parameters.allFrames && targets.size() != 1)
        return makeUnexpected("the requested frame or document is ambiguous"_s);
    return targets;
}

std::expected<URL, String> webExtensionTabScriptPermissionURL(const WebExtensionTabScriptTarget& target, bool matchAboutBlank)
{
    for (size_t i = target.documents.size(); i; --i) {
        const auto& url = target.documents[i - 1].url;
        if (url.isValid() && (url.protocolIsInHTTPFamily() || url.protocolIs("file"_s)))
            return url;
        if (!matchAboutBlank || (!url.isAboutBlank() && !url.isAboutSrcDoc()))
            return makeUnexpected("scripts cannot be injected into this document"_s);
        if (i == 1)
            return makeUnexpected("scripts cannot be injected into a top-level blank document"_s);
    }
    return makeUnexpected("the target document is unavailable"_s);
}

std::expected<WebExtensionTabScriptTarget, String> refreshWebExtensionTabScriptTarget(const WebExtensionTabScriptTarget& target, const FrameTreeNodeData& root)
{
    if (target.documents.isEmpty())
        return makeUnexpected("the target document is unavailable"_s);

    WebExtensionTabScriptTarget refreshed { target.pageIdentifier, { } };
    const FrameTreeNodeData* node = &root;
    const FrameInfoData* parent = nullptr;
    for (size_t i = 0; i < target.documents.size(); ++i) {
        const auto& expected = target.documents[i];
        if (!node || !validFrame(node->info, target.pageIdentifier, parent)
            || node->info.frameID != expected.frameIdentifier
            || *node->info.documentID != expected.documentIdentifier)
            return makeUnexpected("the target document changed before script execution"_s);

        refreshed.documents.append({ node->info.frameID, *node->info.documentID, node->info.request.url() });
        parent = &node->info;
        if (i + 1 < target.documents.size()) {
            const auto nextFrameIdentifier = target.documents[i + 1].frameIdentifier;
            const FrameTreeNodeData* next = nullptr;
            for (const auto& child : node->children) {
                if (child.info.frameID != nextFrameIdentifier)
                    continue;
                if (next)
                    return makeUnexpected("the target document is ambiguous"_s);
                next = &child;
            }
            node = next;
        }
    }
    return refreshed;
}

}
#endif
