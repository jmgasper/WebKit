/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionMenuClickHandlerStore.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionMenuItemParameters.h"
#include "JSWebExtensionTabParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include <WebCore/ContextDestructionObserverInlines.h>
#include <WebCore/Document.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/UserGestureIndicator.h>
namespace WebKit {
Ref<WebExtensionMenuClickHandlerStore> WebExtensionMenuClickHandlerStore::create(WebCore::ScriptExecutionContext& context, WebCore::FrameIdentifier frameIdentifier, WebExtensionContextProxy& extensionContext)
{
    auto result = adoptRef(*new WebExtensionMenuClickHandlerStore(context, frameIdentifier, extensionContext));
    result->suspendIfNeeded();
    extensionContext.addMenusClickHandlerStore(result.get());
    return result;
}
WebExtensionMenuClickHandlerStore::WebExtensionMenuClickHandlerStore(WebCore::ScriptExecutionContext& context, WebCore::FrameIdentifier frameIdentifier, WebExtensionContextProxy& extensionContext)
    : ActiveDOMObject(&context)
    , m_frameIdentifier(frameIdentifier)
    , m_contextIdentifier(extensionContext.identifier())
    , m_extensionContext(extensionContext)
{
}
WebExtensionMenuClickHandlerStore::~WebExtensionMenuClickHandlerStore()
{
    clear();
}
bool WebExtensionMenuClickHandlerStore::matches(WebCore::ScriptExecutionContext& context, WebCore::FrameIdentifier frameIdentifier, WebExtensionContextIdentifier contextIdentifier) const
{
    return scriptExecutionContext() == &context && m_frameIdentifier == frameIdentifier && m_contextIdentifier == contextIdentifier;
}
bool WebExtensionMenuClickHandlerStore::add(const WTF::UUID& identifier, Ref<WebExtensionCallbackHandler>&& callback)
{
    if (isContextStopped() || m_extensionContext->identifier() != m_contextIdentifier || !identifier.isValid() || m_handlers.contains(identifier))
        return false;
    bool wasEmpty = m_handlers.isEmpty();
    m_handlers.add(identifier, WTF::move(callback));
    if (wasEmpty)
        WebProcess::singleton().send(Messages::WebExtensionContext::AddListener(m_frameIdentifier, WebExtensionEventListenerType::MenusOnClicked, WebExtensionContentWorldType::Main), m_contextIdentifier);
    return true;
}
void WebExtensionMenuClickHandlerStore::remove(const WTF::UUID& identifier)
{
    if (m_handlers.remove(identifier) && m_handlers.isEmpty())
        WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(m_frameIdentifier, WebExtensionEventListenerType::MenusOnClicked, WebExtensionContentWorldType::Main, 1), m_contextIdentifier);
}
void WebExtensionMenuClickHandlerStore::clear()
{
    if (m_handlers.isEmpty())
        return;
    m_handlers.clear();
    WebProcess::singleton().send(Messages::WebExtensionContext::RemoveListener(m_frameIdentifier, WebExtensionEventListenerType::MenusOnClicked, WebExtensionContentWorldType::Main, 1), m_contextIdentifier);
}
void WebExtensionMenuClickHandlerStore::stop()
{
    Ref protectedThis { *this };
    clear();
}
void WebExtensionMenuClickHandlerStore::contextDestroyed()
{
    Ref protectedThis { *this };
    clear();
    ActiveDOMObject::contextDestroyed();
}
void WebExtensionMenuClickHandlerStore::dispatch(const WTF::UUID& identifier, const WebExtensionMenuItemParameters& parameters, bool wasChecked, const WebExtensionMenuItemContextParameters& details, const std::optional<WebExtensionTabParameters>& tab)
{
    Ref protectedThis { *this };
    RefPtr callback = m_handlers.get(identifier);
    RefPtr frame = WebProcess::singleton().webFrame(m_frameIdentifier);
    RefPtr coreFrame = frame ? frame->coreLocalFrame() : nullptr;
    if (!callback || !coreFrame || !isAllowedToRunScript() || m_extensionContext->identifier() != m_contextIdentifier)
        return;
    auto context = callback->globalContext();
    Protected<JSValueRef> info(context, toWebAPIMenuClickInfo(context, parameters, wasChecked, details));
    if (!info || !JSValueIsObject(context, info.get()))
        return;
    Protected<JSValueRef> tabValue(context, tab ? toWebAPI(context, *tab) : JSValueMakeUndefined(context));
    WebCore::UserGestureIndicator gesture(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame->document()));
    callback->call(info.get(), tabValue.get());
}
void WebExtensionContextProxy::clearMenusClickHandlerStores()
{
    Vector<Ref<WebExtensionMenuClickHandlerStore>> stores;
    m_menuClickHandlerStores.forEach([&](auto& store) { stores.append(store); });
    for (auto& store : stores)
        store->clear();
}
void WebExtensionContextProxy::addMenusClickHandlerStore(WebExtensionMenuClickHandlerStore& store)
{
    m_menuClickHandlerStores.add(store);
}
void WebExtensionContextProxy::removeMenusClickHandlers(const Vector<WTF::UUID>& identifiers)
{
    // Removal can release the last reference to a store through a JS callback.
    Vector<Ref<WebExtensionMenuClickHandlerStore>> stores;
    m_menuClickHandlerStores.forEach([&](auto& store) { stores.append(store); });
    for (auto& store : stores) {
        for (auto& identifier : identifiers)
            store->remove(identifier);
    }
}
}
#endif
