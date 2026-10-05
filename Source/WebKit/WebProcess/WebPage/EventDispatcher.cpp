/*
 * Copyright (C) 2011, 2014-2015 Apple Inc. All rights reserved.
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
#include "EventDispatcher.h"

#include "EventDispatcherMessages.h"
#include "MomentumEventDispatcher.h"
#include "RemoteWebTouchEvent.h"
#include "WebEventConversion.h"
#include "WebPage.h"
#include "WebProcess.h"
#include "WebProcessProxyMessages.h"
#include "WebTouchEvent.h"
#include "WebWheelEvent.h"
#include <WebCore/DisplayUpdate.h>
#include <WebCore/Document.h>
#include <WebCore/Element.h>
#include <WebCore/Frame.h>
#include <WebCore/HandleUserInputEventResult.h>
#include <WebCore/KeyboardScrollingAnimator.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/NodeInlines.h>
#include <WebCore/Page.h>
#include <WebCore/PageInlines.h>
#include <WebCore/WheelEventTestMonitor.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/SystemTracing.h>
#include <wtf/text/StringBuilder.h>

#if PLATFORM(HAIKU)
#include <cstdio>
#include <cstdlib>
#endif

#if ENABLE(ASYNC_SCROLLING)
#include <WebCore/AsyncScrollingCoordinator.h>
#endif

#include <WebCore/DisplayRefreshMonitorManager.h>

#if ENABLE(IOS_TOUCH_EVENTS)
#include <WebCore/RemoteUserInputEventData.h>
#endif

#if ENABLE(SCROLLING_THREAD)
#include <WebCore/ScrollingThread.h>
#include <WebCore/ScrollingTreeNode.h>
#include <WebCore/ThreadedScrollingTree.h>
#endif

namespace WebKit {
using namespace WebCore;

#if PLATFORM(HAIKU)
static bool traceWheelRoute()
{
    static bool enabled = [] {
        const char* value = std::getenv("SUMMIT_WHEEL_ROUTE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}

// A wheel listener that may call preventDefault() makes every wheel event
// wait for the page's main thread, which is busy while the page loads: seven
// of nine news and video sites scrolled that way, in steps of up to 0.6 s.
// WebKit lets a gesture go on without the main thread once its first event
// was not prevented, but tells gestures by the phases of a trackpad, which a
// mouse wheel does not have. So a burst of wheel events stands for one here,
// as in Chrome: notches that follow one another within 300 ms with the same
// modifiers. The first of a burst is answered by the page; if the page let
// it scroll, the rest scroll on the scrolling thread and reach the page's
// listeners as events they cannot cancel. A page that has not answered the
// first within 50 ms is not waited for, as WebKit does not wait longer for
// the first event of a trackpad's gesture - unless the page has cancelled a
// wheel event before: a map that zooms with the wheel, or a page that
// scrolls by itself, is always waited for, however busy it is.
// SUMMIT_WHEEL_BURSTS=0 turns all of this off.
struct WheelBurstHaiku {
    MonotonicTime lastEventTime;
    MonotonicTime awaitingAnswerSince;
    OptionSet<WebEventModifier> modifiers;
    bool awaitingAnswer { false };
    bool scrollsFreely { false };
    bool pageCancels { false };
};
static Lock s_wheelBurstsLock;
static HashMap<PageIdentifier, WheelBurstHaiku>& wheelBurstsHaiku() WTF_REQUIRES_LOCK(s_wheelBurstsLock)
{
    static NeverDestroyed<HashMap<PageIdentifier, WheelBurstHaiku>> bursts;
    return bursts;
}

static bool wheelBurstsEnabledHaiku()
{
    static bool enabled = [] {
        const char* value = std::getenv("SUMMIT_WHEEL_BURSTS");
        return !value || value[0] != '0' || value[1];
    }();
    return enabled;
}

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
static Seconds haikuScrollRefreshInterval();
#endif

#endif

EventDispatcher::EventDispatcher(WebProcess& process)
    : m_process(process)
    , m_queue(WorkQueue::create("com.apple.WebKit.EventDispatcher"_s, WorkQueue::QOS::UserInteractive))
    , m_recentWheelEventDeltaFilter(WheelEventDeltaFilter::create())
#if ENABLE(MOMENTUM_EVENT_DISPATCHER)
    , m_momentumEventDispatcher(WTF::makeUnique<MomentumEventDispatcher>(*this))
    , m_observerID(DisplayLinkObserverID::generate())
#endif
{
#if PLATFORM(HAIKU) && ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
    // Scroll animations that did not start from a wheel event handled on the
    // scrolling thread would otherwise wait for one to start the refresh
    // pulses. The dispatcher lives as long as the process.
    ThreadedScrollingTree::setScrollAnimationStartedCallback([this] {
        m_queue->dispatch([this] {
            scheduleHaikuScrollRefresh();
        });
    });
#endif
}

EventDispatcher::~EventDispatcher()
{
    ASSERT_NOT_REACHED();
}

void EventDispatcher::ref() const
{
    m_process->ref();
}

void EventDispatcher::deref() const
{
    m_process->deref();
}

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
void EventDispatcher::addScrollingTreeForPage(WebPage& webPage)
{
    Locker locker { m_scrollingTreesLock };

    ASSERT(webPage.scrollingCoordinator());
    ASSERT(!m_scrollingTrees.contains(webPage.identifier()));

    Ref scrollingCoordinator = downcast<AsyncScrollingCoordinator>(*webPage.scrollingCoordinator());
    Ref scrollingTree = downcast<ThreadedScrollingTree>(*scrollingCoordinator->scrollingTree());
    m_scrollingTrees.set(webPage.identifier(), WTF::move(scrollingTree));
}

void EventDispatcher::removeScrollingTreeForPage(WebPage& webPage)
{
    Locker locker { m_scrollingTreesLock };
    ASSERT(m_scrollingTrees.contains(webPage.identifier()));

    m_scrollingTrees.remove(webPage.identifier());
#if PLATFORM(HAIKU)
    Locker burstLocker { s_wheelBurstsLock };
    wheelBurstsHaiku().remove(webPage.identifier());
#endif
}
#endif

void EventDispatcher::initializeConnection(IPC::Connection& connection)
{
    connection.addMessageReceiver(m_queue.get(), *this, Messages::EventDispatcher::messageReceiverName());
}

void EventDispatcher::internalWheelEvent(PageIdentifier pageID, const WebWheelEvent& wheelEvent, RectEdges<WebCore::RubberBandingBehavior> rubberBandableEdges, CompletionHandler<void(bool)>&& completionHandler)
{
    auto processingSteps = OptionSet<WebCore::WheelEventProcessingSteps> { WheelEventProcessingSteps::SynchronousScrolling, WheelEventProcessingSteps::BlockingDOMEventDispatch };

    ensureOnMainRunLoop([pageID] {
        if (RefPtr webPage = WebProcess::singleton().webPage(pageID)) {
            if (RefPtr corePage = webPage->corePage()) {
                if (CheckedPtr keyboardScrollingAnimator = corePage->currentKeyboardScrollingAnimator())
                    keyboardScrollingAnimator->stopScrollingImmediately();
            }
        }
    });

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
    do {
        auto platformWheelEvent = platform(wheelEvent);
#if PLATFORM(COCOA)
        m_recentWheelEventDeltaFilter->updateFromEvent(platformWheelEvent);
        if (WheelEventDeltaFilter::shouldApplyFilteringForEvent(platformWheelEvent))
            platformWheelEvent = m_recentWheelEventDeltaFilter->eventCopyWithFilteredDeltas(platformWheelEvent);
        else if (WheelEventDeltaFilter::shouldIncludeVelocityForEvent(platformWheelEvent))
            platformWheelEvent = m_recentWheelEventDeltaFilter->eventCopyWithVelocity(platformWheelEvent);
#endif

        Locker locker { m_scrollingTreesLock };
        RefPtr scrollingTree = m_scrollingTrees.get(pageID);
        if (!scrollingTree) {
#if PLATFORM(HAIKU)
            if (traceWheelRoute())
                std::fprintf(stderr, "Summit wheel route: no tree, main thread\n");
#endif
            dispatchWheelEventViaMainThread(pageID, wheelEvent, processingSteps, WTF::move(completionHandler));
            break;
        }

        // FIXME: It's pretty horrible that we're updating the back/forward state here.
        // WebCore should always know the current state and know when it changes so the
        // scrolling tree can be notified.
        // We only need to do this at the beginning of the gesture.
        if (platformWheelEvent.phase() == PlatformWheelEventPhase::Began)
            scrollingTree->setClientAllowedMainFrameRubberBandableEdges(rubberBandableEdges);

        auto processingSteps = scrollingTree->determineWheelEventProcessing(platformWheelEvent);
        bool useMainThreadForScrolling = processingSteps.contains(WheelEventProcessingSteps::SynchronousScrolling);

#if !PLATFORM(COCOA)
        // Deliver continuing scroll gestures directly to the scrolling thread until the end.
        if ((platformWheelEvent.phase() == PlatformWheelEventPhase::Changed || platformWheelEvent.phase() == PlatformWheelEventPhase::Ended)
            && scrollingTree->isUserScrollInProgressAtEventLocation(platformWheelEvent))
            useMainThreadForScrolling = false;
#endif

#if PLATFORM(HAIKU)
        bool continuesBurst = false;
        bool answersEarly = false;
        if (wheelBurstsEnabledHaiku()) {
            static constexpr Seconds longestPauseInBurst = 300_ms;
            auto now = MonotonicTime::now();
            Locker burstLocker { s_wheelBurstsLock };
            auto& burst = wheelBurstsHaiku().ensure(pageID, [] {
                return WheelBurstHaiku { };
            }).iterator->value;
            static constexpr Seconds longestWaitForAnswer = 50_ms;
            if (now - burst.lastEventTime > longestPauseInBurst || burst.modifiers != wheelEvent.modifiers()) {
                burst.scrollsFreely = false;
                burst.awaitingAnswer = false;
            }
            burst.lastEventTime = now;
            burst.modifiers = wheelEvent.modifiers();
            if (useMainThreadForScrolling && processingSteps.contains(WheelEventProcessingSteps::BlockingDOMEventDispatch)) {
                if (!burst.scrollsFreely && !burst.pageCancels && burst.awaitingAnswer && now - burst.awaitingAnswerSince > longestWaitForAnswer)
                    burst.scrollsFreely = true;
                answersEarly = !burst.pageCancels;
                if (burst.scrollsFreely) {
                    continuesBurst = true;
                    useMainThreadForScrolling = false;
                    processingSteps = { WheelEventProcessingSteps::AsyncScrolling, WheelEventProcessingSteps::NonBlockingDOMEventDispatch };
                } else if (!burst.awaitingAnswer) {
                    burst.awaitingAnswer = true;
                    burst.awaitingAnswerSince = now;
                }
            }
        }
        if (traceWheelRoute())
            std::fprintf(stderr, "Summit wheel route: tree=1 sync=%d blockingDOM=%d route=%s phase=%d burst=%d\n",
                processingSteps.contains(WheelEventProcessingSteps::SynchronousScrolling),
                processingSteps.contains(WheelEventProcessingSteps::BlockingDOMEventDispatch),
                useMainThreadForScrolling ? "main" : "scrolling", int(platformWheelEvent.phase()), continuesBurst);
#endif

        scrollingTree->willProcessWheelEvent();

#if PLATFORM(HAIKU)
        // The UI process sends the next wheel event when this one has been
        // answered. While the first notch of a burst waits for a main thread
        // that is busy, the notches that could scroll without it would wait
        // in the UI process: it is told after 50 ms that this one is taken
        // care of, which it is.
        if (answersEarly && useMainThreadForScrolling && processingSteps.contains(WheelEventProcessingSteps::BlockingDOMEventDispatch)) {
            struct Answer : ThreadSafeRefCounted<Answer> {
                Lock lock;
                CompletionHandler<void(bool)> handler WTF_GUARDED_BY_LOCK(lock);
            };
            Ref answer = adoptRef(*new Answer);
            {
                Locker answerLocker { answer->lock };
                answer->handler = WTF::move(completionHandler);
            }
            auto answerOnce = [answer](bool handled) {
                CompletionHandler<void(bool)> handler;
                {
                    Locker answerLocker { answer->lock };
                    handler = std::exchange(answer->handler, nullptr);
                }
                if (handler)
                    handler(handled);
            };
            m_queue->dispatchAfter(50_ms, [answerOnce] {
                answerOnce(true);
            });
            completionHandler = CompletionHandler<void(bool)>([answerOnce](bool handled) {
                answerOnce(handled);
            }, CompletionHandlerCallThread::AnyThread);
        }
#endif

#if PLATFORM(HAIKU)
        auto dispatchedAt = MonotonicTime::now();
#endif
        ScrollingThread::dispatch([scrollingTree, wheelEvent = Ref<const WebWheelEvent> { wheelEvent }, platformWheelEvent, processingSteps, useMainThreadForScrolling, pageID, this, protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)
#if PLATFORM(HAIKU)
            , dispatchedAt
#endif
            ] mutable {
#if PLATFORM(HAIKU)
            auto startedAt = MonotonicTime::now();
#endif
            if (useMainThreadForScrolling) {
                scrollingTree->willSendEventToMainThread(platformWheelEvent);
                dispatchWheelEventViaMainThread(pageID, wheelEvent, processingSteps, WTF::move(completionHandler));
                scrollingTree->waitForEventToBeProcessedByMainThread(platformWheelEvent);
                return;
            }

            auto result = scrollingTree->handleWheelEvent(platformWheelEvent, processingSteps);

#if PLATFORM(HAIKU)
            // Only a wheel event that actually moves a scrolling tree needs
            // display-refresh pulses. In particular, wheel events at the end
            // of a finite feed must not keep requesting compositions.
            if (result.wasHandled && haikuScrollRefreshInterval()) {
                m_queue->dispatch([this, protectedThis = Ref { *this }] {
                    scheduleHaikuScrollRefresh();
                });
            }
#endif

#if PLATFORM(HAIKU)
            if (traceWheelRoute())
                std::fprintf(stderr, "Summit wheel result: handled=%d needsMain=%d sync=%d blockingDOM=%d queued=%.1f took=%.1f\n",
                    result.wasHandled, result.needsMainThreadProcessing(),
                    result.steps.contains(WheelEventProcessingSteps::SynchronousScrolling),
                    result.steps.contains(WheelEventProcessingSteps::BlockingDOMEventDispatch),
                    (startedAt - dispatchedAt).milliseconds(), (MonotonicTime::now() - startedAt).milliseconds());
#endif

            if (result.needsMainThreadProcessing()) {
                if (result.steps.contains(WheelEventProcessingSteps::SynchronousScrolling)) {
                    dispatchWheelEventViaMainThread(pageID, wheelEvent, result.steps, WTF::move(completionHandler));
                    return;
                }
                dispatchWheelEventViaMainThread(pageID, wheelEvent, result.steps, CompletionHandler<void(bool)>([](bool) { }, CompletionHandlerCallThread::AnyThread));
            }

            // If we scrolled on the scrolling thread (even if we send the event to the main thread for passive event handlers)
            // respond to the UI process that the event was handled.
            completionHandler(result.wasHandled);
        });
    } while (false);
#else
    UNUSED_PARAM(rubberBandableEdges);

    dispatchWheelEventViaMainThread(pageID, wheelEvent, processingSteps, WTF::move(completionHandler));
#endif
}

void EventDispatcher::wheelEvent(PageIdentifier pageID, Ref<WebWheelEvent>&& wheelEvent, RectEdges<WebCore::RubberBandingBehavior> rubberBandableEdges, CompletionHandler<void(bool)>&& completionHandler)
{
#if ENABLE(MOMENTUM_EVENT_DISPATCHER)
    if (m_momentumEventDispatcher->handleWheelEvent(pageID, wheelEvent, rubberBandableEdges)) {
        completionHandler(true);
        return;
    }
#endif
    internalWheelEvent(pageID, wheelEvent, rubberBandableEdges, WTF::move(completionHandler));
}

#if PLATFORM(HAIKU) && ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
static Seconds haikuScrollRefreshInterval()
{
    static Seconds interval = [] {
        // Haiku has no display link, so these pulses are what advances layer
        // positions and scroll animations on the scrolling thread. They run
        // only while a scrolling tree is active. SUMMIT_SCROLL_REFRESH_TIMER=0
        // turns them off; 16 to 100 sets the interval in milliseconds.
        const char* value = std::getenv("SUMMIT_SCROLL_REFRESH_TIMER");
        if (!value || !value[0])
            return 16_ms;
        char* end = nullptr;
        auto milliseconds = std::strtol(value, &end, 10);
        if (*end || milliseconds < 0)
            return 0_s;
        if (milliseconds == 1)
            milliseconds = 16;
        if (milliseconds < 16 || milliseconds > 100)
            return 0_s;
        return Seconds::fromMilliseconds(milliseconds);
    }();
    return interval;
}

void EventDispatcher::scheduleHaikuScrollRefresh()
{
    // Haiku has no display link. Keep layer positions in step with wheel
    // scrolling, but stop the timer as soon as the scrolling tree is inactive.
    auto interval = haikuScrollRefreshInterval();
    if (!interval || m_haikuScrollRefreshScheduled)
        return;

    m_haikuScrollRefreshScheduled = true;
    m_queue->dispatchAfter(interval, [this, protectedThis = Ref { *this }] {
        refreshHaikuScrollingTrees();
    });
}

void EventDispatcher::refreshHaikuScrollingTrees()
{
    bool hasActiveTree = false;
    Vector<PageIdentifier> pagesToProbe;
    static bool traceDOM = [] {
        const char* value = std::getenv("SUMMIT_SCROLL_DOM_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    {
        Locker locker { m_scrollingTreesLock };
        for (auto& entry : m_scrollingTrees) {
            Ref scrollingTree = entry.value;
            if (!scrollingTree->hasRecentActivity())
                continue;
            hasActiveTree = true;
            scrollingTree->displayDidRefresh(scrollingTree->displayID());
        }
        if (traceDOM && !hasActiveTree) {
            for (auto& entry : m_scrollingTrees)
                pagesToProbe.append(entry.key);
        }
    }

    if (!hasActiveTree) {
        m_haikuScrollRefreshScheduled = false;
        if (!pagesToProbe.isEmpty()) {
            RunLoop::mainSingleton().dispatch([pagesToProbe = WTF::move(pagesToProbe)] {
                for (auto pageID : pagesToProbe) {
                    RefPtr webPage = WebProcess::singleton().webPage(pageID);
                    if (!webPage)
                        continue;
                    RefPtr frame = dynamicDowncast<LocalFrame>(webPage->corePage()->mainFrame());
                    RefPtr document = frame ? frame->document() : nullptr;
                    if (!document)
                        continue;
                    for (unsigned y : { 200u, 400u }) {
                        RefPtr element = document->elementFromPoint(400, y);
                        if (!element) {
                            std::fprintf(stderr, "Summit scroll DOM: y=%u element=none\n", y);
                            continue;
                        }
                        StringBuilder path;
                        for (RefPtr ancestor = element; ancestor && path.length() < 160; ancestor = ancestor->parentElement()) {
                            if (!path.isEmpty())
                                path.append('>');
                            path.append(ancestor->tagName());
                        }
                        auto rect = element->boundingClientRect();
                        auto pathUTF8 = path.toString().utf8();
                        std::fprintf(stderr, "Summit scroll DOM: y=%u path=%s rect=%.0f,%.0f,%.0f,%.0f\n", y,
                            reinterpret_cast<const char*>(pathUTF8.data()), rect.x(), rect.y(), rect.width(), rect.height());
                    }
                }
            });
        }
        return;
    }

    m_queue->dispatchAfter(haikuScrollRefreshInterval(), [this, protectedThis = Ref { *this }] {
        refreshHaikuScrollingTrees();
    });
}
#endif

#if ENABLE(MAC_GESTURE_EVENTS)
void EventDispatcher::gestureEvent(FrameIdentifier frameID, PageIdentifier pageID, Ref<WebGestureEvent>&& gestureEvent, CompletionHandler<void(std::optional<WebEventType>, bool, std::optional<RemoteUserInputEventData>)>&& completionHandler)
{
    RunLoop::mainSingleton().dispatch([this, frameID, pageID, gestureEvent = WTF::move(gestureEvent), completionHandler = WTF::move(completionHandler)] mutable {
        dispatchGestureEvent(frameID, pageID, gestureEvent, WTF::move(completionHandler));
    });
}
#endif

#if ENABLE(IOS_TOUCH_EVENTS)
TouchEventData::TouchEventData(WebCore::FrameIdentifier frameID, Ref<WebTouchEvent>&& event, CompletionHandler<void(bool, std::optional<RemoteWebTouchEvent>)>&& completionHandler)
    : frameID(frameID)
    , event(WTF::move(event))
{
    completionHandlers.append(WTF::move(completionHandler));
}

TouchEventData::TouchEventData(TouchEventData&&) = default;

TouchEventData::~TouchEventData() = default;

TouchEventData& TouchEventData::operator=(TouchEventData&&) = default;

void EventDispatcher::takeQueuedTouchEventsForPage(const WebPage& webPage, UniqueRef<TouchEventQueue>& destinationQueue)
{
    Locker locker { m_touchEventsLock };

    if (auto queue = m_touchEvents.take(webPage.identifier()); queue)
        destinationQueue = makeUniqueRefFromNonNullUniquePtr(WTF::move(queue));
}

void EventDispatcher::touchEvent(PageIdentifier pageID, FrameIdentifier frameID, Ref<WebTouchEvent>&& touchEvent, CompletionHandler<void(bool, std::optional<RemoteWebTouchEvent>)>&& completionHandler)
{
    bool updateListWasEmpty;
    {
        Locker locker { m_touchEventsLock };
        updateListWasEmpty = m_touchEvents.isEmpty();
        auto addResult = m_touchEvents.add(pageID, makeUniqueRef<TouchEventQueue>());
        if (addResult.isNewEntry)
            addResult.iterator->value->append({ frameID, WTF::move(touchEvent), WTF::move(completionHandler) });
        else {
            auto& queuedEvents = addResult.iterator->value;
            ASSERT(!queuedEvents->isEmpty());
            auto& touchEventData = queuedEvents->last();
            // Coalesce touch move events.
            if (touchEvent->type() == WebEventType::TouchMove && touchEventData.event->type() == WebEventType::TouchMove) {
                auto coalescedEvents = Vector<Ref<WebTouchEvent>> { };
                coalescedEvents.appendVector(queuedEvents->last().event->coalescedEvents());
                coalescedEvents.appendVector(touchEvent->coalescedEvents());

                // A copy: the caller's event must not be mutated now that events are shared.
                Ref touchEventWithCoalescedEvents = touchEvent->copy();
                touchEventWithCoalescedEvents->setCoalescedEvents(coalescedEvents);

                // Preserve coalesced completion handlers so their state transitions are not lost.
                queuedEvents->last().frameID = frameID;
                queuedEvents->last().event = WTF::move(touchEventWithCoalescedEvents);
                queuedEvents->last().completionHandlers.append(WTF::move(completionHandler));
            } else
                queuedEvents->append({ frameID, WTF::move(touchEvent), WTF::move(completionHandler) });
        }
    }

    if (updateListWasEmpty) {
        RunLoop::mainSingleton().dispatch([weakThis = WeakPtr { *this }] {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->dispatchTouchEvents();
        });
    }
}

void EventDispatcher::dispatchTouchEvents()
{
    TraceScope traceScope(DispatchTouchEventsStart, DispatchTouchEventsEnd);

    HashMap<PageIdentifier, UniqueRef<TouchEventQueue>> localCopy;
    {
        Locker locker { m_touchEventsLock };
        localCopy.swap(m_touchEvents);
    }

    for (auto& slot : localCopy) {
        if (RefPtr webPage = WebProcess::singleton().webPage(slot.key))
            webPage->dispatchAsynchronousTouchEvents(WTF::move(slot.value));
        else {
            for (auto& data : slot.value.get()) {
                for (auto& completionHandler : data.completionHandlers)
                    completionHandler(false, std::nullopt);
            }
            ASSERT_NOT_REACHED();
        }
    }
}
#elif ENABLE(COORDINATED_TOUCH_EVENTS)
void EventDispatcher::dispatchTouchEventViaMainThread(WebCore::PageIdentifier pageID, Ref<WebTouchEvent>&& touchEvent, CompletionHandler<void(WebEventType, bool)>&& completionHandler)
{
    RunLoop::mainSingleton().dispatch([protectedThis = Ref { *this }, pageID, touchEvent = WTF::move(touchEvent), completionHandler = WTF::move(completionHandler)] mutable {
        RefPtr webPage = WebProcess::singleton().webPage(pageID);
        bool handled = false;
        if (webPage)
            handled = webPage->dispatchTouchEvent(touchEvent.copyRef());
        if (completionHandler)
            completionHandler(touchEvent->type(), handled);
    });
}

void EventDispatcher::touchEvent(PageIdentifier pageID, FrameIdentifier frameID, Ref<WebTouchEvent>&& touchEvent, CompletionHandler<void(WebEventType, bool)>&& completionHandler)
{
    RefPtr<WebCore::ThreadedScrollingTree> scrollingTree;
    {
        Locker locker { m_scrollingTreesLock };
        scrollingTree = m_scrollingTrees.get(pageID);
    }

    if (!scrollingTree) {
        dispatchTouchEventViaMainThread(pageID, WTF::move(touchEvent), WTF::move(completionHandler));
        return;
    }

    auto trackingType = scrollingTree->eventTrackingTypeForTouchEvent(platform(touchEvent));

    if (trackingType == TrackingType::NotTracking) {
        completionHandler(touchEvent->type(), false);
        return;
    }
    if (trackingType == TrackingType::Asynchronous) {
        completionHandler(touchEvent->type(), false);
        dispatchTouchEventViaMainThread(pageID, WTF::move(touchEvent), nullptr);
        return;
    }
    dispatchTouchEventViaMainThread(pageID, WTF::move(touchEvent), WTF::move(completionHandler));
}
#endif

void EventDispatcher::dispatchWheelEventViaMainThread(WebCore::PageIdentifier pageID, const WebWheelEvent& wheelEvent, OptionSet<WheelEventProcessingSteps> processingSteps, CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(!RunLoop::isMain());
    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }, pageID, wheelEvent = Ref<const WebWheelEvent> { wheelEvent }, steps = processingSteps - WheelEventProcessingSteps::AsyncScrolling, completionHandler = WTF::move(completionHandler)] mutable {
        dispatchWheelEvent(pageID, wheelEvent, steps, WTF::move(completionHandler));
    });
}

void EventDispatcher::dispatchWheelEvent(PageIdentifier pageID, const WebWheelEvent& wheelEvent, OptionSet<WheelEventProcessingSteps> processingSteps, CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(RunLoop::isMain());

    RefPtr webPage = WebProcess::singleton().webPage(pageID);

    bool handled = false;
    if (webPage && webPage->mainFrame()) {
#if PLATFORM(HAIKU)
        auto [result, handling] = webPage->wheelEvent(webPage->mainFrame()->frameID(), wheelEvent, processingSteps);
        handled = result.wasHandled();
        // The page has answered an event it could cancel: what it decided
        // holds for the rest of the burst.
        if (wheelBurstsEnabledHaiku() && processingSteps.contains(WheelEventProcessingSteps::BlockingDOMEventDispatch)) {
            Locker burstLocker { s_wheelBurstsLock };
            auto it = wheelBurstsHaiku().find(pageID);
            if (it != wheelBurstsHaiku().end()) {
                bool cancelled = handling.contains(EventHandling::DefaultPrevented);
                it->value.scrollsFreely = !cancelled;
                it->value.awaitingAnswer = false;
                if (cancelled)
                    it->value.pageCancels = true;
            }
        }
#else
        auto [result, _] = webPage->wheelEvent(webPage->mainFrame()->frameID(), wheelEvent, processingSteps);
        handled = result.wasHandled();
#endif
    }

    completionHandler(handled);
}

#if ENABLE(MAC_GESTURE_EVENTS)
void EventDispatcher::dispatchGestureEvent(FrameIdentifier frameID, PageIdentifier pageID, const WebGestureEvent& gestureEvent, CompletionHandler<void(std::optional<WebEventType>, bool, std::optional<RemoteUserInputEventData>)>&& completionHandler)
{
    ASSERT(RunLoop::isMain());

    RefPtr webPage = WebProcess::singleton().webPage(pageID);
    if (!webPage)
        return completionHandler(gestureEvent.type(), false, std::nullopt);

    webPage->gestureEvent(frameID, gestureEvent, WTF::move(completionHandler));
}
#endif

void EventDispatcher::notifyScrollingTreesDisplayDidRefresh(PlatformDisplayID displayID)
{
#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
    Locker locker { m_scrollingTreesLock };
    for (Ref scrollingTree : m_scrollingTrees.values())
        scrollingTree->displayDidRefresh(displayID);
#endif
}

#if HAVE(DISPLAY_LINK)
void EventDispatcher::displayDidRefresh(PlatformDisplayID displayID, const DisplayUpdate& displayUpdate, bool sendToMainThread)
{
    tracePoint(DisplayRefreshDispatchingToMainThread, displayID, sendToMainThread);

    ASSERT(!RunLoop::isMain());

#if ENABLE(MOMENTUM_EVENT_DISPATCHER)
    m_momentumEventDispatcher->displayDidRefresh(displayID);
#endif

    notifyScrollingTreesDisplayDidRefresh(displayID);

    if (!sendToMainThread)
        return;

    RunLoop::mainSingleton().dispatch([displayID, displayUpdate]() {
        DisplayRefreshMonitorManager::sharedManager().displayDidRefresh(displayID, displayUpdate);
    });
}
#endif

void EventDispatcher::pageScreenDidChange(PageIdentifier pageID, PlatformDisplayID displayID, std::optional<unsigned> nominalFramesPerSecond)
{
#if ENABLE(MOMENTUM_EVENT_DISPATCHER)
    m_momentumEventDispatcher->pageScreenDidChange(pageID, displayID, nominalFramesPerSecond);
#else
    UNUSED_PARAM(pageID);
    UNUSED_PARAM(displayID);
#endif
}

#if ENABLE(MOMENTUM_EVENT_DISPATCHER)
void EventDispatcher::setScrollingAccelerationCurve(PageIdentifier pageID, std::optional<ScrollingAccelerationCurve>&& curve)
{
    m_momentumEventDispatcher->setScrollingAccelerationCurve(pageID, WTF::move(curve));
}

void EventDispatcher::handleSyntheticWheelEvent(WebCore::PageIdentifier pageIdentifier, const WebWheelEvent& event, WebCore::RectEdges<WebCore::RubberBandingBehavior> rubberBandableEdges)
{
    internalWheelEvent(pageIdentifier, event, rubberBandableEdges, [](bool) { });
}

void EventDispatcher::startDisplayDidRefreshCallbacks(WebCore::PlatformDisplayID displayID)
{
    protect(WebProcess::singleton().parentProcessConnection())->send(Messages::WebProcessProxy::StartDisplayLink(m_observerID, displayID, WebCore::FullSpeedFramesPerSecond), 0);
}

void EventDispatcher::stopDisplayDidRefreshCallbacks(WebCore::PlatformDisplayID displayID)
{
    protect(WebProcess::singleton().parentProcessConnection())->send(Messages::WebProcessProxy::StopDisplayLink(m_observerID, displayID), 0);
}

#if ENABLE(MOMENTUM_EVENT_DISPATCHER_TEMPORARY_LOGGING)
void EventDispatcher::flushMomentumEventLoggingSoon()
{
    queue().dispatchAfter(1_s, [weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->m_momentumEventDispatcher->flushLog();
    });
}
#endif
#endif // ENABLE(MOMENTUM_EVENT_DISPATCHER)

} // namespace WebKit
