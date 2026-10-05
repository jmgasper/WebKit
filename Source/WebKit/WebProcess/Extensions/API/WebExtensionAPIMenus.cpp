/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIMenus.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionMenuItemParameters.h"
#include "JSWebExtensionTabParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionMenuClickHandlerStore.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include <JavaScriptCore/APICast.h>
#include <WebCore/Document.h>
#include <WebCore/JSDOMGlobalObject.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/UserGestureIndicator.h>
namespace WebKit {
WebExtensionAPIMenus::~WebExtensionAPIMenus() = default;

RefPtr<WebExtensionMenuClickHandlerStore> WebExtensionAPIMenus::ensureClickHandlerStore(JSContextRef context, WebFrame& frame, String& exception)
{
    auto* global = dynamicDowncast<WebCore::JSDOMGlobalObject>(toJS(JSContextGetGlobalContext(context)));
    RefPtr executionContext = global ? global->scriptExecutionContext() : nullptr;
    if (!executionContext || executionContext->activeDOMObjectsAreStopped()) {
        exception = "menu callback document is no longer active"_s;
        return nullptr;
    }
    for (auto& store : m_clickHandlerStores) {
        if (store->matches(*executionContext, frame.frameID(), extensionContext().identifier()))
            return store.ptr();
    }
    auto store = WebExtensionMenuClickHandlerStore::create(*executionContext, frame.frameID(), extensionContext());
    m_clickHandlerStores.append(store.copyRef());
    return store;
}

static void finishMenuRequest(WebExtensionCallbackHandler& callback, const std::expected<void, WebExtensionError>& result)
{
    if (!result)
        callback.reportError(result.error().isEmpty() ? "The menu request could not reach the extension context."_s : result.error());
    else
        callback.call();
}

JSValueRef WebExtensionAPIMenus::createMenu(WebPage&, WebFrame& frame, JSContextRef context, JSValueRef properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto input = parseWebExtensionMenuItemInput(context, properties, WebExtensionMenuItemOperation::Create, extensionContext().supportsManifestVersion(3), frame.url());
    if (!input) {
        exception = input.error();
        return JSValueMakeUndefined(context);
    }
    RefPtr<WebExtensionMenuClickHandlerStore> store;
    std::optional<WTF::UUID> token;
    if (input->clickHandler) {
        store = ensureClickHandlerStore(context, frame, exception);
        if (!store)
            return JSValueMakeUndefined(context);
        token = WTF::UUID::createVersion4();
        if (!store->add(*token, WebExtensionCallbackHandler::create(context, input->clickHandler.get(), runtime()))) {
            exception = "menu callback document is no longer active"_s;
            return JSValueMakeUndefined(context);
        }
    }
    Protected<JSValueRef> identifier(JSContextGetGlobalContext(context), toWebAPIMenuIdentifier(context, input->parameters.identifier));
    // Register before IPC. Success never registers again: a cleanup notification
    // received before this reply must not resurrect a removed handler.
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::MenusCreateWithCallback(input->parameters, input->explicitIdentifier, token),
        [protectedThis = Ref { *this }, callback = WTF::move(callback), store, token](std::expected<void, WebExtensionError>&& result) {
            if (!result && store && token)
                store->remove(*token);
            finishMenuRequest(callback.get(), result);
        }, extensionContext().identifier());
    return identifier.get();
}

void WebExtensionAPIMenus::update(WebPage&, WebFrame& frame, JSContextRef context, JSValueRef identifierValue, JSValueRef properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto identifier = parseWebExtensionMenuIdentifier(context, identifierValue);
    auto input = parseWebExtensionMenuItemInput(context, properties, WebExtensionMenuItemOperation::Update, extensionContext().supportsManifestVersion(3), frame.url());
    if (!identifier || !input) {
        exception = !identifier ? identifier.error() : input.error();
        return;
    }
    RefPtr<WebExtensionMenuClickHandlerStore> store;
    std::optional<WTF::UUID> token;
    if (input->clickHandler) {
        store = ensureClickHandlerStore(context, frame, exception);
        if (!store)
            return;
        token = WTF::UUID::createVersion4();
        if (!store->add(*token, WebExtensionCallbackHandler::create(context, input->clickHandler.get(), runtime()))) {
            exception = "menu callback document is no longer active"_s;
            return;
        }
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::MenusUpdateWithCallback(*identifier, input->parameters, input->clickHandlerSpecified, token),
        [protectedThis = Ref { *this }, callback = WTF::move(callback), store, token](std::expected<void, WebExtensionError>&& result) {
            if (!result && store && token)
                store->remove(*token);
            finishMenuRequest(callback.get(), result);
        }, extensionContext().identifier());
}

void WebExtensionAPIMenus::remove(JSContextRef context, JSValueRef value, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto identifier = parseWebExtensionMenuIdentifier(context, value);
    if (!identifier) {
        exception = identifier.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::MenusRemove(*identifier),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) { finishMenuRequest(callback.get(), result); }, extensionContext().identifier());
}
void WebExtensionAPIMenus::removeAll(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::MenusRemoveAll(),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) { finishMenuRequest(callback.get(), result); }, extensionContext().identifier());
}
WebExtensionAPIEvent& WebExtensionAPIMenus::onClicked()
{
    if (!m_onClicked)
        m_onClicked = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::MenusOnClicked);
    return *m_onClicked;
}
void WebExtensionContextProxy::dispatchMenusClickedEvent(const WebExtensionMenuItemParameters& parameters, bool wasChecked, const WebExtensionMenuItemContextParameters& details, const std::optional<WebExtensionTabParameters>& tab, const std::optional<WTF::UUID>& token)
{
    if (!hasPermission("menus"_s) && !hasPermission("contextMenus"_s))
        return;
    if (token && token->isValid()) {
        Vector<Ref<WebExtensionMenuClickHandlerStore>> stores;
        m_menuClickHandlerStores.forEach([&](auto& store) { stores.append(store); });
        for (auto& store : stores)
            store->dispatch(*token, parameters, wasChecked, details, tab);
    }
    enumerateFramesAndNamespaceObjects([&](auto& frame, auto& object) {
        RefPtr coreFrame = frame.coreLocalFrame();
        WebCore::UserGestureIndicator gesture(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));
        auto listeners = object.menus().onClicked().listeners();
        for (RefPtr listener : listeners) {
            auto context = listener->globalContext();
            Protected<JSValueRef> info(context, toWebAPIMenuClickInfo(context, parameters, wasChecked, details));
            if (!info || !JSValueIsObject(context, info.get()))
                continue;
            Protected<JSValueRef> tabValue(context, tab ? toWebAPI(context, *tab) : JSValueMakeUndefined(context));
            listener->call(info.get(), tabValue.get());
        }
    });
}
}
#endif
