/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionUtilities.h"

// The native browser has no registry of native messaging hosts yet, so every
// application is unknown. Answer the way Firefox does for a missing host instead
// of leaving the caller without a reply: sendNativeMessage() rejects, and a
// connectNative() port disconnects with runtime.lastError and port.error set.

namespace WebKit {

static String missingNativeApplicationError(const String& applicationID)
{
    // Extension script may only see printable text it supplied itself.
    auto name = applicationID.contains('\0') ? emptyString() : applicationID.left(256);
    return makeString("No such native application "_s, name);
}

void WebExtensionContext::runtimeSendNativeMessage(const String& applicationID, const String&, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completionHandler)
{
    completionHandler(makeUnexpected(missingNativeApplicationError(applicationID)));
}

void WebExtensionContext::runtimeConnectNative(const String& applicationID, WebExtensionPortChannelIdentifier channelIdentifier, WebPageProxyIdentifier pageProxyIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    constexpr auto sourceContentWorldType = WebExtensionContentWorldType::Main;
    constexpr auto targetContentWorldType = WebExtensionContentWorldType::Native;

    // Add 1 for the starting port here so disconnect will balance with a decrement.
    addPorts(sourceContentWorldType, targetContentWorldType, channelIdentifier, { pageProxyIdentifier });

    // The port itself was created; the missing host is reported through it.
    completionHandler({ });

    // No native endpoint will ever join this channel. Tell the page's port now,
    // with the reason, and drop anything it already queued for the host.
    firePortDisconnectEventIfNeeded(targetContentWorldType, sourceContentWorldType, channelIdentifier, missingNativeApplicationError(applicationID));
    clearQueuedPortMessages(targetContentWorldType, channelIdentifier);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
