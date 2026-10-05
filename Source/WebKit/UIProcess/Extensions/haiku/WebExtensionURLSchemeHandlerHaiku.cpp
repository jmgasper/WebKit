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
#include "WebExtensionURLSchemeHandler.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "APIData.h"
#include "APIFrameHandle.h"
#include "APIFrameInfo.h"
#include "APIPageConfiguration.h"
#include "WebExtension.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionTab.h"
#include "WebFrameProxy.h"
#include "WebPageProxy.h"
#include "WebURLSchemeTask.h"
#include <Errors.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/RunLoop.h>

namespace WebKit {

using namespace WebCore;

WebExtensionURLSchemeHandler::WebExtensionURLSchemeHandler(WebExtensionController& controller)
    : m_webExtensionController(controller)
{
}

void WebExtensionURLSchemeHandler::platformStartTask(WebPageProxy& page, WebURLSchemeTask& task)
{
    ASSERT(RunLoop::isMain());
    m_pendingTasks.add(task);
    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }, task = Ref { task }, page = Ref { page }] {
        // stop() can remove a task before this queued operation reaches the main loop.
        if (!m_pendingTasks.contains(task))
            return;

        URL frameDocumentURL = task->frameInfo().request().url();
        URL requestURL = task->request().url();

        if (frameDocumentURL.isEmpty() || frameDocumentURL.isAboutBlank()) {
            frameDocumentURL = task->request().firstPartyForCookies();
            if (!task->frameInfo().isMainFrame()) {
                if (RefPtr parentFrameHandle = task->frameInfo().parentFrameHandle()) {
                    if (RefPtr parent = WebFrameProxy::webFrame(parentFrameHandle->frameID()))
                        frameDocumentURL = parent->url();
                }
            }
        }

        auto denyAccess = [&] {
            // Missing/disabled extensions and inaccessible resources have identical errors.
            // Distinguishing them would expose which extensions the user has installed.
            task->didComplete(ResourceError("WebExtensionResourceError"_s, B_PERMISSION_DENIED, requestURL,
                "The extension resource is not accessible."_s, ResourceError::Type::AccessControl));
        };

        RefPtr controller = m_webExtensionController.get();
        if (!controller) {
            denyAccess();
            return;
        }

        RefPtr context = controller->extensionContext(requestURL);
        if (!context) {
            denyAccess();
            return;
        }

        Ref extension = context->extension();
#if ENABLE(INSPECTOR_EXTENSIONS)
        if (!frameDocumentURL.protocolIs("inspector-resource"_s) && !protocolHostAndPortAreEqual(frameDocumentURL, requestURL))
#else
        if (!protocolHostAndPortAreEqual(frameDocumentURL, requestURL))
#endif
        {
            if (!extension->isWebAccessibleResource(requestURL, frameDocumentURL)) {
                denyAccess();
                return;
            }
        }

        bool loadingExtensionMainFrame = false;
        if (task->frameInfo().isMainFrame() && requestURL == frameDocumentURL) {
            if (!context->isURLForThisExtension(page->configuration().requiredWebExtensionBaseURL())) {
                task->didComplete(ResourceError("WebExtensionResourceError"_s, B_NOT_ALLOWED, requestURL,
                    "The page is not configured for this extension."_s));
                return;
            }
            loadingExtensionMainFrame = true;
        }

        auto result = extension->resourceDataForPath(requestURL.path().toString());
        if (!result) {
            context->recordErrorIfNeeded(result.error());
            task->didComplete(ResourceError("WebExtensionResourceError"_s, B_ENTRY_NOT_FOUND, requestURL,
                "The extension resource could not be read."_s));
            return;
        }
        Ref resourceData = result.value();

        if (loadingExtensionMainFrame) {
            if (auto tab = context->getTab(page->identifier()))
                context->addExtensionTabPage(page, *tab);
        }

        auto mimeType = extension->resourceMIMETypeForPath(requestURL.path().toString());
        resourceData = context->localizedResourceData(WTF::move(resourceData), mimeType).releaseNonNull();

        ResourceResponse response(URL { requestURL }, String { mimeType }, resourceData->size(), { });
        response.setHTTPStatusCode(200);
        response.setHTTPHeaderField(HTTPHeaderName::AccessControlAllowOrigin, "*"_s);
        response.setHTTPHeaderField(HTTPHeaderName::ContentSecurityPolicy, extension->contentSecurityPolicy());
        response.setHTTPHeaderField(HTTPHeaderName::ContentLength, String::number(resourceData->size()));
        response.setHTTPHeaderField(HTTPHeaderName::ContentType, mimeType);

        if (task->didReceiveResponse(WTF::move(response)) != WebURLSchemeTask::ExceptionType::None)
            return;
        if (task->didReceiveData(SharedBuffer::create(resourceData->span())) != WebURLSchemeTask::ExceptionType::None)
            return;
        task->didComplete({ });
    });
}

void WebExtensionURLSchemeHandler::platformStopTask(WebPageProxy&, WebURLSchemeTask& task)
{
    ASSERT(RunLoop::isMain());
    m_pendingTasks.remove(task);
}

void WebExtensionURLSchemeHandler::platformTaskCompleted(WebURLSchemeTask& task)
{
    ASSERT(RunLoop::isMain());
    m_pendingTasks.remove(task);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
