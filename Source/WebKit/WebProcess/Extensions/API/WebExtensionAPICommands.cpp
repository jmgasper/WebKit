/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionAPICommands.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "JSWebExtensionTabParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionCommandParameters.h"
#include "WebExtensionCommandSettings.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionTabParameters.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include <WebCore/Document.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/UserGestureIndicator.h>

namespace WebKit {

void WebExtensionAPICommands::getAll(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CommandsGetAll(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](Vector<WebExtensionCommandParameters> commands) {
        auto result = JSON::Array::create();
        for (auto& command : commands) {
            auto object = JSON::Object::create();
            object->setString("name"_s, command.identifier);
            object->setString("description"_s, command.description);
            object->setString("shortcut"_s, command.shortcut);
            result->pushObject(WTF::move(object));
        }
        callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString(result->toJSONString()).get()));
    }, extensionContext().identifier());
}

void WebExtensionAPICommands::update(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    auto update = parseWebExtensionCommandUpdate(details.get());
    if (!update) {
        outExceptionString = update.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CommandsUpdate(update->name, update->description, update->shortcut), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPICommands::reset(const String& name, Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::CommandsReset(name), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

WebExtensionAPIEvent& WebExtensionAPICommands::onCommand()
{
    if (!m_onCommand)
        m_onCommand = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::CommandsOnCommand);
    return *m_onCommand;
}

WebExtensionAPIEvent& WebExtensionAPICommands::onChanged()
{
    if (!m_onChanged)
        m_onChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::CommandsOnChanged);
    return *m_onChanged;
}

void WebExtensionContextProxy::dispatchCommandsCommandEvent(const String& identifier, const std::optional<WebExtensionTabParameters>& tabParameters)
{
    enumerateFramesAndNamespaceObjects([&](WebFrame& frame, auto& namespaceObject) {
        RefPtr coreFrame = frame.coreLocalFrame();
        WebCore::UserGestureIndicator gesture(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));
        namespaceObject.commands().onCommand().invokeListenersWithParametersArgument(identifier, tabParameters);
    });
}

void WebExtensionContextProxy::dispatchCommandsChangedEvent(const String& identifier, const String& oldShortcut, const String& newShortcut)
{
    auto details = JSON::Object::create();
    details->setString("name"_s, identifier);
    details->setString("oldShortcut"_s, oldShortcut);
    details->setString("newShortcut"_s, newShortcut);
    auto json = details->toJSONString();
    enumerateNamespaceObjects([&](auto& namespaceObject) {
        namespaceObject.commands().onChanged().invokeListenersWithJSONArgument(json);
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
