/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIIdle.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionUtilities.h"
#include <InterfaceDefs.h>
#include <wtf/JSONValues.h>

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/idle
static constexpr double minimumDetectionInterval = 15;
static constexpr Seconds pollInterval { 5_s };

static String stateForInterval(Seconds interval)
{
    // The native input server reports the time since the last keyboard or pointer event.
    return Seconds::fromMicroseconds(idle_time()) >= interval ? "idle"_s : "active"_s;
}
void WebExtensionAPIIdle::queryState(double detectionIntervalInSeconds, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!std::isfinite(detectionIntervalInSeconds) || detectionIntervalInSeconds < minimumDetectionInterval) {
        outExceptionString = toErrorString(nullString(), "detectionIntervalInSeconds"_s, "it must be at least 15 seconds"_s);
        return;
    }
    callback->call(toJSValueRef(callback->globalContext(), stateForInterval(Seconds { detectionIntervalInSeconds })));
}
void WebExtensionAPIIdle::setDetectionInterval(double intervalInSeconds, String& outExceptionString)
{
    if (!std::isfinite(intervalInSeconds) || intervalInSeconds < minimumDetectionInterval) {
        outExceptionString = toErrorString(nullString(), "intervalInSeconds"_s, "it must be at least 15 seconds"_s);
        return;
    }
    m_detectionInterval = Seconds { intervalInSeconds };
}
WebExtensionAPIEvent& WebExtensionAPIIdle::onStateChanged()
{
    if (!m_onStateChanged) {
        m_onStateChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::IdleOnStateChanged);
        m_lastState = stateForInterval(m_detectionInterval);
        // The timer is owned by this object and cannot outlive it.
        m_timer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "WebExtensionAPIIdle::poll"_s, [this] {
            poll();
        });
        m_timer->startRepeating(pollInterval);
    }
    return *m_onStateChanged;
}
void WebExtensionAPIIdle::poll()
{
    auto state = stateForInterval(m_detectionInterval);
    if (state == m_lastState)
        return;
    m_lastState = state;
    if (m_onStateChanged)
        m_onStateChanged->invokeListenersWithJSONArgument(JSON::Value::create(state)->toJSONString());
}
}
#endif
