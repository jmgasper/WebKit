/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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
 *  THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 *  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "config.h"
#include "ScriptedAnimationController.h"

#include "DOMWrapperWorld.h"
#include "DocumentPage.h"
#include "FrameDestructionObserverInlines.h"
#include "InspectorInstrumentation.h"
#include "JSRequestAnimationFrameCallback.h"
#include "Logging.h"
#include "OpportunisticTaskScheduler.h"
#include "Page.h"
#include "RenderObjectStyle.h"
#include "RequestAnimationFrameCallback.h"
#include "ScriptController.h"
#include "Settings.h"
#include "UserGestureIndicator.h"
#include <wtf/SystemTracing.h>

#if PLATFORM(HAIKU)
#include <cstdlib>
#include <cstring>
#endif

namespace WebCore {

ScriptedAnimationController::ScriptedAnimationController(Document& document)
    : m_document(document)
{
}

ScriptedAnimationController::~ScriptedAnimationController() = default;

void ScriptedAnimationController::suspend()
{
    ++m_suspendCount;
}

void ScriptedAnimationController::resume()
{
    // It would be nice to put an ASSERT(m_suspendCount > 0) here, but in WK1 resume() can be called
    // even when suspend hasn't (if a tab was created in the background).
    if (m_suspendCount > 0)
        --m_suspendCount;

    if (!m_suspendCount && m_callbackDataList.size())
        scheduleAnimation();
}

Page* ScriptedAnimationController::page() const
{
    return m_document ? m_document->page() : nullptr;
}

Seconds ScriptedAnimationController::interval() const
{
    return preferredScriptedAnimationInterval();
}

Seconds ScriptedAnimationController::preferredScriptedAnimationInterval() const
{
    auto* page = this->page();
    if (!page)
        return FullSpeedAnimationInterval;

    return preferredFrameInterval(throttlingReasons(), page->displayNominalFramesPerSecond(), page->settings().preferPageRenderingUpdatesNear60FPSEnabled());
}

OptionSet<ThrottlingReason> ScriptedAnimationController::throttlingReasons() const
{
    if (auto* page = this->page())
        return page->throttlingReasons() | m_throttlingReasons;

    return m_throttlingReasons;
}

bool ScriptedAnimationController::isThrottledRelativeToPage() const
{
    if (RefPtr page = this->page())
        return preferredScriptedAnimationInterval() > page->preferredRenderingUpdateInterval();
    return false;
}

bool ScriptedAnimationController::shouldRescheduleRequestAnimationFrame(ReducedResolutionSeconds timestamp) const
{
    LOG_WITH_STREAM(RequestAnimationFrame, stream << "ScriptedAnimationController::shouldRescheduleRequestAnimationFrame - throttled relative to page " << isThrottledRelativeToPage()
        << ", last delta " << (timestamp - m_lastAnimationFrameTimestamp).milliseconds() << "ms, preferred interval " << preferredScriptedAnimationInterval().milliseconds() << ")");

#if PLATFORM(HAIKU)
    // Haiku has no display link: rendering updates also come from layer
    // flushes, style changes and frames' loads, and each ran the callbacks,
    // so pages saw 63-66 frames a second (xbitlabs.com's FPS test measured
    // that as the refresh rate and the 60 frames a second after it as drops).
    // Callbacks that ran less than half a frame ago wait for the next
    // refresh. Half, not a whole frame: a refresh whose update had to wait
    // for the compositor can be followed by one that did not, closer than a
    // frame, and skipping that one would drop a frame.
    // SUMMIT_RAF_MIN_INTERVAL_MS sets the gap (0 runs them in every update).
    static const Seconds minimumInterval = [] {
        const char* value = getenv("SUMMIT_RAF_MIN_INTERVAL_MS");
        return value && *value ? Seconds::fromMilliseconds(std::max(0.0, atof(value))) : Seconds(-1);
    }();
    Seconds gap = minimumInterval >= 0_s ? minimumInterval : preferredScriptedAnimationInterval() / 2;
    if (gap > 0_s && m_lastAnimationFrameTimestamp && timestamp - m_lastAnimationFrameTimestamp < gap)
        return true;
#endif
    return timestamp <= m_lastAnimationFrameTimestamp || (isThrottledRelativeToPage() && (timestamp - m_lastAnimationFrameTimestamp < preferredScriptedAnimationInterval()));
}

ScriptedAnimationController::CallbackId ScriptedAnimationController::registerCallback(Ref<RequestAnimationFrameCallback>&& callback)
{
    CallbackId callbackId = ++m_nextCallbackId;
    callback->m_firedOrCancelled = false;
    callback->m_id = callbackId;
    RefPtr<ImminentlyScheduledWorkScope> workScope;
    if (RefPtr page = this->page())
        workScope = page->opportunisticTaskScheduler().makeScheduledWorkScope();
    m_callbackDataList.append({ WTF::move(callback), UserGestureIndicator::currentUserGesture(), WTF::move(workScope) });

    if (RefPtr document = m_document.get())
        InspectorInstrumentation::didRequestAnimationFrame(*document, callbackId);

    if (!m_suspendCount)
        scheduleAnimation();
    return callbackId;
}

void ScriptedAnimationController::cancelCallback(CallbackId callbackId)
{
    bool cancelled = m_callbackDataList.removeFirstMatching([callbackId](auto& data) {
        if (data.callback->m_id != callbackId)
            return false;
        data.callback->m_firedOrCancelled = true;
        return true;
    });

    if (cancelled && m_document)
        InspectorInstrumentation::didCancelAnimationFrame(*protect(m_document), callbackId);
}

void ScriptedAnimationController::serviceRequestAnimationFrameCallbacks(ReducedResolutionSeconds timestamp)
{
    if (!m_callbackDataList.size() || m_suspendCount)
        return;

    if (shouldRescheduleRequestAnimationFrame(timestamp)) {
        LOG_WITH_STREAM(RequestAnimationFrame, stream << "ScriptedAnimationController::serviceRequestAnimationFrameCallbacks - rescheduling (page update interval " << (page() ? page()->preferredRenderingUpdateInterval() : 0_s) << ", raf interval " << preferredScriptedAnimationInterval() << ")");
        scheduleAnimation();
        return;
    }
    
    TraceScope tracingScope(RAFCallbackStart, RAFCallbackEnd);

    auto highResNowMs = timestamp.milliseconds();

    LOG_WITH_STREAM(RequestAnimationFrame, stream << "ScriptedAnimationController::serviceRequestAnimationFrameCallbacks at " << highResNowMs << " (throttling reasons " << throttlingReasons() << ", preferred interval " << preferredScriptedAnimationInterval().milliseconds() << "ms)");

    // First, generate a list of callbacks to consider.  Callbacks registered from this point
    // on are considered only for the "next" frame, not this one.
    Vector<CallbackData> callbackDataList(m_callbackDataList);

    // Invoking callbacks may detach elements from our document, which clears the document's
    // reference to us, so take a defensive reference.
    Ref protectedThis { *this };
    Ref document = *m_document;

    RefPtr frame = document->frame();
    if (!frame)
        return;

    CheckedRef script = frame->script();
    if (script->isPaused())
        return;

    bool canExecuteScriptsInAnyWorld = false;
    for (auto& [callback, userGestureTokenToForward, scheduledWorkScope] : callbackDataList) {
        if (callback->m_firedOrCancelled)
            continue;

        DOMWrapperWorld* world = nullptr;
        if (RefPtr jsCallback = dynamicDowncast<JSRequestAnimationFrameCallback>(callback.get())) {
            if (auto* globalObject = jsCallback->callbackData()->globalObject())
                world = &globalObject->world();
        }
        if (!script->canExecuteScripts(ReasonForCallingCanExecuteScripts::AboutToExecuteScript, world))
            continue;

        canExecuteScriptsInAnyWorld = true;
        callback->m_firedOrCancelled = true;

        if (userGestureTokenToForward && Ref { *userGestureTokenToForward }->hasExpired(UserGestureToken::maximumIntervalForUserGestureForwarding))
            userGestureTokenToForward = nullptr;
        UserGestureIndicator gestureIndicator(userGestureTokenToForward);

        auto identifier = callback->m_id;
        InspectorInstrumentation::willFireAnimationFrame(document, identifier);
        Ref { callback }->invoke(highResNowMs);
        InspectorInstrumentation::didFireAnimationFrame(document, identifier);
    }

    if (!canExecuteScriptsInAnyWorld)
        return;

    // Remove any callbacks we fired from the list of pending callbacks.
    m_callbackDataList.removeAllMatching([](auto& data) {
        return data.callback->m_firedOrCancelled;
    });

    m_lastAnimationFrameTimestamp = timestamp;

    if (m_callbackDataList.size())
        scheduleAnimation();
}

void ScriptedAnimationController::scheduleAnimation()
{
    if (RefPtr page = this->page())
        page->scheduleRenderingUpdate(RenderingUpdateStep::AnimationFrameCallbacks);
}


}
