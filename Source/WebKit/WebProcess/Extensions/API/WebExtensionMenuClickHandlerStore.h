/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionWrapper.h"
#include "WebExtensionContextIdentifier.h"
#include <WebCore/ActiveDOMObject.h>
#include <WebCore/FrameIdentifier.h>
#include <wtf/HashMap.h>
#include <wtf/UUID.h>
namespace WebKit {
class WebExtensionContextProxy;
struct WebExtensionMenuItemParameters;
struct WebExtensionMenuItemContextParameters;
struct WebExtensionTabParameters;

// Owns callbacks for one document. The context proxy indexes this object weakly,
// independently of mutable JavaScript namespace properties.
class WebExtensionMenuClickHandlerStore final : public RefCounted<WebExtensionMenuClickHandlerStore>, public WebCore::ActiveDOMObject {
public:
    static Ref<WebExtensionMenuClickHandlerStore> create(WebCore::ScriptExecutionContext&, WebCore::FrameIdentifier, WebExtensionContextProxy&);
    ~WebExtensionMenuClickHandlerStore();
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    bool matches(WebCore::ScriptExecutionContext&, WebCore::FrameIdentifier, WebExtensionContextIdentifier) const;
    bool add(const WTF::UUID&, Ref<WebExtensionCallbackHandler>&&);
    void remove(const WTF::UUID&);
    void clear();
    void dispatch(const WTF::UUID&, const WebExtensionMenuItemParameters&, bool wasChecked, const WebExtensionMenuItemContextParameters&, const std::optional<WebExtensionTabParameters>&);

private:
    WebExtensionMenuClickHandlerStore(WebCore::ScriptExecutionContext&, WebCore::FrameIdentifier, WebExtensionContextProxy&);
    void stop() final;
    void contextDestroyed() final;

    const WebCore::FrameIdentifier m_frameIdentifier;
    const WebExtensionContextIdentifier m_contextIdentifier;
    const Ref<WebExtensionContextProxy> m_extensionContext;
    HashMap<WTF::UUID, Ref<WebExtensionCallbackHandler>> m_handlers;
};
}
#endif
