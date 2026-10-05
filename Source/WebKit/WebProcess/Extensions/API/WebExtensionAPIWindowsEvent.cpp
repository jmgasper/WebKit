/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIWindowsEvent.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionWindowQueryParser.h"
#include "WebProcess.h"
namespace WebKit {
void WebExtensionAPIWindowsEvent::addListener(WebCore::FrameIdentifier frame, RefPtr<WebExtensionCallbackHandler> listener, RefPtr<JSON::Value> filter, String& exception)
{
    auto query = parseWebExtensionWindowQuery(filter.get(), WebExtensionWindowQueryKind::Event);
    if (!query) {
        exception = query.error();
        return;
    }
    if (!hasExtensionContext()) {
        exception = "extension context is unavailable"_s;
        return;
    }
    m_frameIdentifier = frame;
    m_listeners.append({ WTF::move(listener), query->types });
    WebProcess::singleton().send(Messages::WebExtensionContext::AddListener(frame, m_type, contentWorldType()), extensionContext().identifier());
}
void WebExtensionAPIWindowsEvent::removeListener(WebCore::FrameIdentifier frame, RefPtr<WebExtensionCallbackHandler> listener)
{
    auto count = m_listeners.removeAllMatching([&](auto& entry) { return entry.first->callbackFunction() == listener->callbackFunction(); });
    if (!count)
        return;
    ASSERT(frame == m_frameIdentifier);
    if (hasExtensionContext())
        WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(frame, m_type, contentWorldType(), count), extensionContext().identifier());
}
bool WebExtensionAPIWindowsEvent::hasListener(RefPtr<WebExtensionCallbackHandler> listener)
{
    return m_listeners.containsIf([&](auto& entry) { return entry.first->callbackFunction() == listener->callbackFunction(); });
}
void WebExtensionAPIWindowsEvent::removeAllListeners()
{
    if (m_listeners.isEmpty())
        return;
    if (hasExtensionContext())
        WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(*m_frameIdentifier, m_type, contentWorldType(), m_listeners.size()), extensionContext().identifier());
    m_listeners.clear();
}
}
#endif
