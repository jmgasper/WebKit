/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "ResourceLoadInfo.h"
#include "WebExtensionMatchPattern.h"
#include "WebExtensionTabIdentifier.h"
#include "WebExtensionWindowIdentifier.h"
#include <wtf/JSONValues.h>

namespace WebKit {

String webExtensionWebRequestResourceType(const ResourceLoadInfo&);
Ref<JSON::Object> webExtensionWebRequestDetails(const ResourceLoadInfo&, WebExtensionTabIdentifier, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);

String webExtensionWebRequestStatusLine(const String& httpVersion, int statusCode, const String& statusText);

class WebExtensionWebRequestFilter {
public:
    static std::expected<WebExtensionWebRequestFilter, String> parse(const JSON::Value&);
    bool matches(const ResourceLoadInfo&, WebExtensionTabIdentifier, WebExtensionWindowIdentifier) const;

private:
    Vector<Ref<WebExtensionMatchPattern>> m_urlPatterns;
    std::optional<HashSet<String>> m_types;
    std::optional<WebExtensionTabIdentifier> m_tab;
    std::optional<WebExtensionWindowIdentifier> m_window;
};

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
