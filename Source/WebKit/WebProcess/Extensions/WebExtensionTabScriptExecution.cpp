/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionTabScriptExecution.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "ContentWorldShared.h"
#include "FrameTreeNodeData.h"
#include "InjectedBundleScriptWorld.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionControllerProxy.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebProcess.h"
#include "WebUserContentController.h"
#include <JavaScriptCore/TopExceptionScope.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSPromise.h>
#include <WebCore/CommonVM.h>
#include <WebCore/Document.h>
#include <WebCore/JSDOMPromise.h>
#include <WebCore/JSDOMWindowBase.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/RunJavaScriptParameters.h>
#include <WebCore/ScriptController.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

class PendingTabScript final : public RefCounted<PendingTabScript>, public CanMakeWeakPtr<PendingTabScript> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(PendingTabScript);
public:
    static void wait(WebPage& page, WebExtensionContextIdentifier context, ContentWorldIdentifier world, const WebExtensionTabScriptTarget& target, WebExtensionTabScriptRunAt runAt, CompletionHandler<void(std::expected<WebExtensionTabScriptTarget, String>&&)>&& completion)
    {
        Ref pending = adoptRef(*new PendingTabScript(page, context, world, target));
        pending->m_runAt = runAt;
        pending->m_readyCompletion = WTF::move(completion);
        pending->poll();
    }

    static void execute(WebPage& page, WebExtensionContextIdentifier context, InjectedBundleScriptWorld& world, const WebExtensionTabScriptTarget& target, const URL& authorizedURL, bool matchAboutBlank, const String& source, const URL& sourceURL, bool userGesture, CompletionHandler<void(WebExtensionTabScriptExecutionResult&&)>&& completion)
    {
        // scripting.executeScript({ world: 'MAIN' }) runs in the page's normal
        // world. The extension's content-script world still identifies the
        // current load for the validity checks below.
        bool mainWorld = world.identifier() == pageContentWorldIdentifier();
        RefPtr loadedExtension = WebExtensionContextProxy::get(context);
        auto validityWorld = mainWorld && loadedExtension && loadedExtension->contentScriptWorldIdentifier()
            ? *loadedExtension->contentScriptWorldIdentifier() : world.identifier();
        Ref pending = adoptRef(*new PendingTabScript(page, context, validityWorld, target));
        pending->m_resultCompletion = WTF::move(completion);
        JSC::JSLockHolder lock(commonVM());
        auto current = pending->currentTarget();
        if (!current) {
            pending->fail(current.error());
            return;
        }
        auto permissionURL = webExtensionTabScriptPermissionURL(*current, matchAboutBlank);
        if (!permissionURL || *permissionURL != authorizedURL) {
            pending->fail("the document URL changed after authorization"_s);
            return;
        }
        RefPtr frame = pending->currentFrame();
        RefPtr coreFrame = frame ? frame->coreLocalFrame() : nullptr;
        if (!coreFrame || !coreFrame->document() || coreFrame->document()->isPluginDocument()) {
            pending->fail("scripts cannot be injected into this document"_s);
            return;
        }
        RefPtr extension = WebExtensionContextProxy::get(context);
        if (!extension || (!mainWorld && !extension->matchesContentScriptWorld(world.coreWorld()))) {
            pending->fail("the extension content world is no longer available"_s);
            return;
        }
        if (!mainWorld)
            extension->setContentScriptWorld(world.coreWorld());
        pending->m_world = world;
        // Evaluate ordinary script source, preserving global declarations and
        // its last-statement completion. Await promises separately below.
        WebCore::RunJavaScriptParameters parameters { source, JSC::SourceTaintedOrigin::Untainted, URL { sourceURL }, false, std::nullopt, userGesture, RemoveTransientActivation::No };
        coreFrame->script().executeAsynchronousUserAgentScriptInWorld(world.coreWorld(), WTF::move(parameters),
            [pending](ValueOrException result) {
                if (!result) {
                    pending->fail(result.error().message);
                    return;
                }
                pending->receiveValue(result.value());
            });
    }

private:
    PendingTabScript(WebPage& page, WebExtensionContextIdentifier context, ContentWorldIdentifier world, const WebExtensionTabScriptTarget& target)
        : m_page(page), m_context(context), m_worldIdentifier(world), m_target(target) { }

    std::expected<WebExtensionTabScriptTarget, String> currentTarget() const
    {
        RefPtr page = m_page.get();
        RefPtr extension = WebExtensionContextProxy::get(m_context);
        RefPtr controller = page ? page->webExtensionControllerProxy() : nullptr;
        if (!page || !extension || !controller || extension->unprivilegedIdentifier() != m_context
            || controller->extensionContext(extension->uniqueIdentifier()) != extension
            || extension->contentScriptWorldIdentifier() != m_worldIdentifier)
            return makeUnexpected("the extension was unloaded or its content world changed"_s);
        return refreshWebExtensionTabScriptTarget(m_target, page->mainWebFrame().frameTreeData());
    }

    RefPtr<WebFrame> currentFrame() const
    {
        if (m_target.documents.isEmpty())
            return nullptr;
        return WebProcess::singleton().webFrame(m_target.documents.last().frameIdentifier);
    }

    void schedulePoll()
    {
        RunLoop::mainSingleton().dispatchAfter(16_ms, [protectedThis = Ref { *this }] { protectedThis->poll(); });
    }

    void poll()
    {
        if (!m_readyCompletion && !m_resultCompletion)
            return;
        JSC::JSLockHolder lock(commonVM());
        auto current = currentTarget();
        if (!current) {
            fail(current.error());
            return;
        }
        if (m_readyCompletion) {
            RefPtr frame = currentFrame();
            RefPtr coreFrame = frame ? frame->coreLocalFrame() : nullptr;
            RefPtr document = coreFrame ? coreFrame->document() : nullptr;
            if (!document || document->isPluginDocument()) {
                fail("the target document is unavailable for script execution"_s);
                return;
            }
            bool ready = m_runAt == WebExtensionTabScriptRunAt::DocumentStart
                || (m_runAt == WebExtensionTabScriptRunAt::DocumentEnd && document->readyState() != Document::ReadyState::Loading)
                || (m_runAt == WebExtensionTabScriptRunAt::DocumentIdle && document->readyState() == Document::ReadyState::Complete);
            if (ready) {
                auto completion = WTF::move(m_readyCompletion);
                completion(WTF::move(*current));
                return;
            }
        }
        schedulePoll();
    }

    void fail(const String& error)
    {
        m_promise = nullptr;
        if (m_readyCompletion) {
            auto completion = WTF::move(m_readyCompletion);
            completion(makeUnexpected(error));
        }
        if (m_resultCompletion) {
            auto completion = WTF::move(m_resultCompletion);
            completion(makeUnexpected(error));
        }
    }

    void receiveValue(JSC::JSValue value, bool didAwait = false)
    {
        if (!m_resultCompletion)
            return;
        auto current = currentTarget();
        if (!current) {
            fail(current.error());
            return;
        }
        RefPtr frame = currentFrame();
        RefPtr coreFrame = frame ? frame->coreLocalFrame() : nullptr;
        if (!coreFrame || !m_world) {
            fail("the script result document is no longer available"_s);
            return;
        }
        auto* global = coreFrame->script().globalObject(m_world->coreWorld());
        auto& vm = global->vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        if (!didAwait) {
            // Async script results assimilate arbitrary thenables, including
            // inherited then methods and native promises with an own then.
            // Use an intrinsic fresh promise; extension code can replace the
            // global Promise constructor or its resolve method.
            auto* promise = JSC::JSPromise::create(vm, global->promiseStructure());
            m_promise = DOMPromise::create(*global, *promise);
            promise->resolve(global, vm, value);
            if (scope.exception()) {
                scope.clearExceptionExceptTermination();
                fail("unable to resolve the script result"_s);
                return;
            }
            auto registered = m_promise->whenSettledWithResult([weakThis = WeakPtr { *this }](JSDOMGlobalObject* global, bool fulfilled, JSC::JSValue result) {
                RefPtr pending = weakThis.get();
                if (!pending || !pending->m_resultCompletion)
                    return;
                if (fulfilled) {
                    pending->receiveValue(result, true);
                    return;
                }
                auto scope = DECLARE_TOP_EXCEPTION_SCOPE(global->vm());
                String error = result.toWTFString(global);
                if (scope.exception()) {
                    scope.clearExceptionExceptTermination();
                    error = "the script returned a rejected promise"_s;
                }
                pending->fail(error);
            });
            if (registered == DOMPromise::IsCallbackRegistered::No) {
                fail("unable to observe the script's promise result"_s);
                return;
            }
            schedulePoll();
            return;
        }
        RefPtr serialized = SerializedScriptValue::create(*global, value, SerializationForStorage::No, SerializationErrorMode::NonThrowing);
        if (scope.exception())
            scope.clearExceptionExceptTermination();
        if (!serialized || serialized->wireBytes().size() > webExtensionMaxMessageLength) {
            fail("the script result cannot be cloned or exceeds the maximum size"_s);
            return;
        }
        m_promise = nullptr;
        auto completion = WTF::move(m_resultCompletion);
        completion(WTF::move(serialized));
    }

    WeakPtr<WebPage> m_page;
    WebExtensionContextIdentifier m_context;
    ContentWorldIdentifier m_worldIdentifier;
    WebExtensionTabScriptTarget m_target;
    WebExtensionTabScriptRunAt m_runAt { WebExtensionTabScriptRunAt::DocumentStart };
    RefPtr<InjectedBundleScriptWorld> m_world;
    RefPtr<DOMPromise> m_promise;
    CompletionHandler<void(std::expected<WebExtensionTabScriptTarget, String>&&)> m_readyCompletion;
    CompletionHandler<void(WebExtensionTabScriptExecutionResult&&)> m_resultCompletion;
};

void WebPage::waitForExtensionTabScript(WebExtensionContextIdentifier context, ContentWorldIdentifier world, const WebExtensionTabScriptTarget& target, WebExtensionTabScriptRunAt runAt, CompletionHandler<void(std::expected<WebExtensionTabScriptTarget, String>&&)>&& completion)
{
    PendingTabScript::wait(*this, context, world, target, runAt, WTF::move(completion));
}

void WebPage::executeExtensionTabScript(WebExtensionContextIdentifier context, const ContentWorldData& worldData, const WebExtensionTabScriptTarget& target, const URL& authorizedURL, bool matchAboutBlank, const String& source, const URL& sourceURL, bool userGesture, CompletionHandler<void(WebExtensionTabScriptExecutionResult&&)>&& completion)
{
    m_userContentController->addContentWorldIfNecessary(worldData);
    RefPtr world = m_userContentController->worldForIdentifier(worldData.identifier);
    if (!world) {
        completion(makeUnexpected("the extension content world is unavailable"_s));
        return;
    }
    PendingTabScript::execute(*this, context, *world, target, authorizedURL, matchAboutBlank, source, sourceURL, userGesture, WTF::move(completion));
}

}
#endif
