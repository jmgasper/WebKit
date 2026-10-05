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
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "FormDataReference.h"
#include "Logging.h"
#include "ResourceLoadInfo.h"
#include "WebErrors.h"
#include "WebExtensionContentRuleListBlockedLoadInfo.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionPermission.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/FormData.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>

namespace WebKit {

static std::optional<WebCore::FrameIdentifier> mainFrameIdentifierForWebRequest(WebPageProxyIdentifier pageIdentifier, const ResourceLoadInfo& resource)
{
    RefPtr page = WebProcessProxy::webPage(pageIdentifier);
    if (!page || page->isClosed())
        return std::nullopt;

    // A response can belong to a provisional or replaced frame. Prefer a live
    // parent that is itself a main frame for this page over the current root.
    if (resource.parentFrameID) {
        RefPtr parent = WebFrameProxy::webFrame(*resource.parentFrameID);
        if (parent && parent->page() == page.get() && parent->isMainFrame())
            return parent->frameID();
    }
    RefPtr mainFrame = page->mainFrame();
    return mainFrame ? std::optional { mainFrame->frameID() } : std::nullopt;
}

bool WebExtensionContext::hasPermissionToSendWebRequestEvent(WebExtensionTab* tab, const URL& resourceURL, const ResourceLoadInfo& loadInfo)
{
    if (!isLoaded() || !tab || !isValidTab(*tab) || !tab->extensionHasAccess())
        return false;

    if (!hasPermission(WebExtensionPermission::webRequest(), tab))
        return false;

    bool isMainFrameNavigation = loadInfo.type == ResourceLoadInfo::Type::Document && !loadInfo.parentFrameID;
    if (!isMainFrameNavigation && !tab->extensionHasPermission())
        return false;

    if (!resourceURL.isEmpty() && (!resourceURL.isValid() || !hasPermission(resourceURL, tab)))
        return false;

    const URL& resourceLoadURL = loadInfo.originalURL;
    if (!resourceLoadURL.isValid() || !hasPermission(resourceLoadURL, tab))
        return false;

    return true;
}

void WebExtensionContext::resourceLoadDidSendRequest(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceRequest& request)
{
    RefPtr tab = getTab(pageID);
    if (!hasPermissionToSendWebRequestEvent(tab.get(), request.url(), loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    std::optional<IPC::FormDataReference> formDataReference;
    if (RefPtr formData = request.httpBody()) {
        RefPtr resolvedFormData = formData->resolveBlobReferences().ptr();
        formDataReference = IPC::FormDataReference { WTF::move(resolvedFormData) };
    }

    constexpr auto beforeRequestType = WebExtensionEventListenerType::WebRequestOnBeforeRequest;
    constexpr auto beforeSendHeadersType = WebExtensionEventListenerType::WebRequestOnBeforeSendHeaders;
    constexpr auto sendHeadersType = WebExtensionEventListenerType::WebRequestOnSendHeaders;

    wakeUpBackgroundContentIfNecessaryToFireEvents({ beforeRequestType, beforeSendHeadersType, sendHeadersType }, [=, this, protectedThis = Ref { *this }] {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), request.url(), loadInfo))
            return;

        sendToProcessesForEvents({ beforeRequestType, beforeSendHeadersType, sendHeadersType }, Messages::WebExtensionContextProxy::ResourceLoadDidSendRequest(tab->identifier(), windowIdentifier, request, loadInfo, formDataReference, mainFrameIdentifier));
    });
}

void WebExtensionContext::resourceLoadDidPerformHTTPRedirection(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& request)
{
    RefPtr tab = getTab(pageID);
    if (!hasPermissionToSendWebRequestEvent(tab.get(), request.url(), loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    constexpr auto headersReceivedType = WebExtensionEventListenerType::WebRequestOnHeadersReceived;
    constexpr auto redirectType = WebExtensionEventListenerType::WebRequestOnBeforeRedirect;

    wakeUpBackgroundContentIfNecessaryToFireEvents({ headersReceivedType, redirectType }, [=, this, protectedThis = Ref { *this }] {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), request.url(), loadInfo))
            return;

        sendToProcessesForEvents({ headersReceivedType, redirectType }, Messages::WebExtensionContextProxy::ResourceLoadDidPerformHTTPRedirection(tab->identifier(), windowIdentifier, response, loadInfo, request, mainFrameIdentifier));
    });

    // After dispatching the redirect events, also dispatch the `didSendRequest` events for the redirection.
    resourceLoadDidSendRequest(pageID, loadInfo, request);
}

void WebExtensionContext::resourceLoadDidReceiveChallenge(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::AuthenticationChallenge& challenge)
{
    RefPtr tab = getTab(pageID);
    if (!hasPermissionToSendWebRequestEvent(tab.get(), URL { }, loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    constexpr auto authRequiredType = WebExtensionEventListenerType::WebRequestOnAuthRequired;

    wakeUpBackgroundContentIfNecessaryToFireEvents({ authRequiredType }, [=, this, protectedThis = Ref { *this }] {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), URL { }, loadInfo))
            return;

        sendToProcessesForEvent(authRequiredType, Messages::WebExtensionContextProxy::ResourceLoadDidReceiveChallenge(tab->identifier(), windowIdentifier, challenge, loadInfo, mainFrameIdentifier));
    });
}

void WebExtensionContext::resourceLoadDidReceiveResponse(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response)
{
    RefPtr tab = getTab(pageID);
    if (!hasPermissionToSendWebRequestEvent(tab.get(), response.url(), loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    constexpr auto headersReceivedType = WebExtensionEventListenerType::WebRequestOnHeadersReceived;
    constexpr auto responseStartedType = WebExtensionEventListenerType::WebRequestOnResponseStarted;

    wakeUpBackgroundContentIfNecessaryToFireEvents({ headersReceivedType, responseStartedType }, [=, this, protectedThis = Ref { *this }] {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), response.url(), loadInfo))
            return;

        sendToProcessesForEvents({ headersReceivedType, responseStartedType }, Messages::WebExtensionContextProxy::ResourceLoadDidReceiveResponse(tab->identifier(), windowIdentifier, response, loadInfo, mainFrameIdentifier));
    });
}

void WebExtensionContext::resourceLoadDidCompleteWithError(WebPageProxyIdentifier pageID, const ResourceLoadInfo& loadInfo, const WebCore::ResourceResponse& response, const WebCore::ResourceError& error)
{
    RefPtr tab = getTab(pageID);

#if PLATFORM(COCOA)
    // If a Fetch or XHR fails due to CORS, prompt the user for permission to the URL
    // if the URL of the frame where the request originated corresponds to this extension.
    // This won't help the failed request, but future requests might succeed if the user
    // grants permission.
    if (error.isAccessControl() && (loadInfo.type == ResourceLoadInfo::Type::Fetch || loadInfo.type == ResourceLoadInfo::Type::XMLHTTPRequest)) {
        RefPtr<WebFrameProxy> originatingFrame = loadInfo.frameID ? WebFrameProxy::webFrame(*loadInfo.frameID) : nullptr;
        if (originatingFrame && isURLForThisExtension(originatingFrame->url())) {
            RELEASE_LOG_ERROR(Extensions, "Requesting permission to access URL due to CORS failure: %{sensitive}s", loadInfo.originalURL.string().utf8().legacyCStringPointer());
            requestPermissionToAccessURLs({ loadInfo.originalURL }, tab, nullptr, GrantOnCompletion::Yes, { PermissionStateOptions::RequestedWithTabsPermission, PermissionStateOptions::IncludeOptionalPermissions });
        }
    }

#endif

    if (!hasPermissionToSendWebRequestEvent(tab.get(), response.url(), loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    constexpr auto errorOccurredType = WebExtensionEventListenerType::WebRequestOnErrorOccurred;
    constexpr auto completedType = WebExtensionEventListenerType::WebRequestOnCompleted;

    wakeUpBackgroundContentIfNecessaryToFireEvents({ errorOccurredType, completedType }, [=, this, protectedThis = Ref { *this }] mutable {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), response.url(), loadInfo))
            return;

        sendToProcessesForEvents({ errorOccurredType, completedType }, Messages::WebExtensionContextProxy::ResourceLoadDidCompleteWithError(tab->identifier(), windowIdentifier, response, error, loadInfo, mainFrameIdentifier));
    });
}

#if ENABLE(CONTENT_EXTENSIONS)
void WebExtensionContext::resourceLoadWasBlockedByDeclarativeNetRequest(WebPageProxyIdentifier pageID, const WebExtensionContentRuleListBlockedLoadInfo& info)
{
    RefPtr tab = getTab(pageID);

    ResourceLoadInfo loadInfo {
        NetworkResourceLoadIdentifier::generate(),
        info.frameID,
        info.parentFrameID,
        { },
        info.url,
        info.httpMethod,
        WallTime::now(),
        false,
        info.type
    };

    if (!hasPermissionToSendWebRequestEvent(tab.get(), info.url, loadInfo))
        return;

    RefPtr window = tab->window();
    auto windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    auto mainFrameIdentifier = mainFrameIdentifierForWebRequest(pageID, loadInfo);

    constexpr auto beforeRequestType = WebExtensionEventListenerType::WebRequestOnBeforeRequest;
    constexpr auto errorOccurredType = WebExtensionEventListenerType::WebRequestOnErrorOccurred;

    auto error = blockedByContentBlockerError(WebCore::ResourceRequest { URL { info.url } });

    wakeUpBackgroundContentIfNecessaryToFireEvents({ beforeRequestType, errorOccurredType }, [=, this, protectedThis = Ref { *this }] mutable {
        // The tab, profile access or permission may have changed while the
        // background context was waking. Retained objects do not grant access.
        if (!hasPermissionToSendWebRequestEvent(tab.get(), info.url, loadInfo))
            return;

        sendToProcessesForEvent(beforeRequestType, Messages::WebExtensionContextProxy::ResourceLoadDidBlockBeforeRequest(tab->identifier(), windowIdentifier, loadInfo, mainFrameIdentifier));
        sendToProcessesForEvent(errorOccurredType, Messages::WebExtensionContextProxy::ResourceLoadDidCompleteWithError(tab->identifier(), windowIdentifier, WebCore::ResourceResponse { }, error, loadInfo, mainFrameIdentifier));
    });
}

#endif

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
