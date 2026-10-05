/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
#include "WebExtensionController.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "WebExtensionContentRuleListBlockedLoadInfo.h"
#include "WebExtensionContext.h"

namespace WebKit {

void WebExtensionController::resourceLoadDidSendRequest(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceRequest& request)
{
    auto contexts = m_extensionContexts;
    for (Ref context : contexts) {
        if (context->extensionController() != this)
            continue;
        context->resourceLoadDidSendRequest(pageID, loadInfo, request);
    }
}

void WebExtensionController::resourceLoadDidPerformHTTPRedirection(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& request)
{
    auto contexts = m_extensionContexts;
    for (Ref context : contexts) {
        if (context->extensionController() != this)
            continue;
        context->resourceLoadDidPerformHTTPRedirection(pageID, loadInfo, response, request);
    }
}

void WebExtensionController::resourceLoadDidReceiveChallenge(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::AuthenticationChallenge& challenge)
{
    auto contexts = m_extensionContexts;
    for (Ref context : contexts) {
        if (context->extensionController() != this)
            continue;
        context->resourceLoadDidReceiveChallenge(pageID, loadInfo, challenge);
    }
}

void WebExtensionController::resourceLoadDidReceiveResponse(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response)
{
    auto contexts = m_extensionContexts;
    for (Ref context : contexts) {
        if (context->extensionController() != this)
            continue;
        context->resourceLoadDidReceiveResponse(pageID, loadInfo, response);
    }
}

void WebExtensionController::resourceLoadDidCompleteWithError(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response, const WebCore::ResourceError& error)
{
    auto contexts = m_extensionContexts;
    for (Ref context : contexts) {
        if (context->extensionController() != this)
            continue;
        context->resourceLoadDidCompleteWithError(pageID, loadInfo, response, error);
    }
}

#if ENABLE(CONTENT_EXTENSIONS)
void WebExtensionController::resourceLoadWasBlockedByContentRuleList(WebPageProxyIdentifier pageID, const WebExtensionContentRuleListBlockedLoadInfo& info)
{
    auto contexts = m_extensionContexts;
    for (auto& identifier : info.blockingContentRuleListIdentifiers) {
        for (Ref context : contexts) {
            if (context->extensionController() == this && context->uniqueIdentifier() == identifier) {
                context->resourceLoadWasBlockedByDeclarativeNetRequest(pageID, info);
                break;
            }
        }
    }
}

#endif

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
