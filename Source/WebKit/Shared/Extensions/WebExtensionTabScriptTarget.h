/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionTabScriptParameters.h"
#include "WebPageProxyIdentifier.h"
#include <WebCore/FrameIdentifier.h>
#include <WebCore/ScriptExecutionContextIdentifier.h>
#include <expected>
#include <wtf/URL.h>
#include <wtf/Vector.h>

namespace WebKit {

struct FrameTreeNodeData;

struct WebExtensionTabScriptDocument {
    WebCore::FrameIdentifier frameIdentifier;
    WebCore::ScriptExecutionContextIdentifier documentIdentifier;
    URL url;
};

struct WebExtensionTabScriptTarget {
    WebPageProxyIdentifier pageIdentifier;
    // Root through target, inclusive. Keep process-qualified document IDs:
    // frame IDs and URLs alone survive replacement of a document.
    Vector<WebExtensionTabScriptDocument> documents;
};

// Capture before requesting permission. The returned order always starts with
// the main frame when it is selected. No extension permissions are granted here.
std::expected<Vector<WebExtensionTabScriptTarget>, String> captureWebExtensionTabScriptTargets(const FrameTreeNodeData&, WebPageProxyIdentifier, const WebExtensionTabScriptParameters&);

// Resolve the URL the caller must authorize against current host, activeTab,
// file and private-access permissions. Reject privileged schemes; blank frames
// can inherit only through their captured parent chain, never from an opener.
std::expected<URL, String> webExtensionTabScriptPermissionURL(const WebExtensionTabScriptTarget&, bool matchAboutBlank);

// After a wait, refresh URLs only if every document in the captured path still
// exists. The caller must check permissions again using the refreshed target.
std::expected<WebExtensionTabScriptTarget, String> refreshWebExtensionTabScriptTarget(const WebExtensionTabScriptTarget&, const FrameTreeNodeData&);

}
#endif
