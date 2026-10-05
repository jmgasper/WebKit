/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <wtf/MainThread.h>

namespace WebKit {

WebExtensionAction& WebExtensionContext::defaultAction()
{
    if (!m_defaultAction)
        m_defaultAction = WebExtensionAction::create(*this);

    return *m_defaultAction;
}

Ref<WebExtensionAction> WebExtensionContext::getAction(WebExtensionWindow* window)
{
    if (!window)
        return defaultAction();

    if (RefPtr windowAction = m_actionWindowMap.get(*window))
        return *windowAction;

    return defaultAction();
}

Ref<WebExtensionAction> WebExtensionContext::getAction(WebExtensionTab* tab)
{
    if (!tab)
        return defaultAction();

    if (RefPtr tabAction = m_actionTabMap.get(*tab))
        return *tabAction;

    return getAction(tab->window().get());
}

Ref<WebExtensionAction> WebExtensionContext::getOrCreateAction(WebExtensionWindow* window)
{
    if (!window)
        return defaultAction();

    return m_actionWindowMap.ensure(*window, [&] {
        return WebExtensionAction::create(*this, *window);
    }).iterator->value;
}

Ref<WebExtensionAction> WebExtensionContext::getOrCreateAction(WebExtensionTab* tab)
{
    if (!tab)
        return defaultAction();

    return m_actionTabMap.ensure(*tab, [&] {
        return WebExtensionAction::create(*this, *tab);
    }).iterator->value;
}

void WebExtensionContext::setNativeActionChangeHandler(RefPtr<NativeActionChangeHandler>&& handler)
{
    ASSERT(isMainThread());
    RELEASE_ASSERT(++m_nativeActionChangeHandlerGeneration);
    m_nativeActionChangeHandler = WTF::move(handler);
}

void WebExtensionContext::nativeActionPropertiesDidChange(WebExtensionAction& action)
{
    ASSERT(isMainThread());
    validateNativeActionPopup();
    RefPtr handler = m_nativeActionChangeHandler;
    if (!isLoaded() || !handler)
        return;

    auto validAction = [&](WebExtensionAction& candidate) {
        if (candidate.extensionContext() != this)
            return false;
        if (candidate.isDefaultAction())
            return m_defaultAction.get() == &candidate;
        if (candidate.isTabAction()) {
            RefPtr tab = candidate.tab();
            if (!tab || !isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess())
                return false;
            RefPtr current = m_actionTabMap.get(*tab);
            return current.get() == &candidate;
        }
        RefPtr window = candidate.window();
        if (!window || !isValidWindow(*window) || !window->extensionHasAccess())
            return false;
        RefPtr current = m_actionWindowMap.get(*window);
        return current.get() == &candidate;
    };
    if (!validAction(action))
        return;

    Vector<Ref<WebExtensionAction>> targets { Ref { action } };
    if (action.isDefaultAction()) {
        for (auto entry : m_actionWindowMap) {
            Ref candidate { entry.value.get() };
            if (validAction(candidate.get()))
                targets.append(WTF::move(candidate));
        }
    }
    if (!action.isTabAction()) {
        RefPtr window = action.window();
        for (auto entry : m_actionTabMap) {
            Ref candidate { entry.value.get() };
            if (!validAction(candidate.get()))
                continue;
            if (action.isDefaultAction() || candidate->tab()->window() == window)
                targets.append(WTF::move(candidate));
        }
    }

    auto generation = m_nativeActionChangeHandlerGeneration;
    auto loadIdentifier = privilegedIdentifier();
    for (Ref target : targets) {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier
            || generation != m_nativeActionChangeHandlerGeneration || m_nativeActionChangeHandler != handler)
            return;
        if (validAction(target))
            handler->run(target);
    }
}

bool WebExtensionContext::shouldDisplayBlockedResourceCountAsBadgeText()
{
    return m_state && m_state->getBoolean("DisplayBlockedResourceCountAsBadgeText"_s).value_or(false);
}

void WebExtensionContext::saveShouldDisplayBlockedResourceCountAsBadgeText(bool display)
{
    if (!m_state)
        m_state = JSON::Object::create();
    m_state->setBoolean("DisplayBlockedResourceCountAsBadgeText"_s, display);
    writeStateToStorage();
}

void WebExtensionContext::incrementActionCountForTab(WebExtensionTab& tab, ssize_t amount)
{
    if (!isLoaded() || !isValidTab(tab) || !tab.isOpen() || !tab.extensionHasAccess()
        || !shouldDisplayBlockedResourceCountAsBadgeText())
        return;
    getOrCreateAction(&tab)->incrementBlockedResourceCount(amount);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
