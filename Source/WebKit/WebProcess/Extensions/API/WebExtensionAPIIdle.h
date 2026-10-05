/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIIdle.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"
#include <wtf/RunLoop.h>

namespace WebKit {
class WebExtensionAPIIdle final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIIdle, idle, idle);
public:
    void queryState(double detectionIntervalInSeconds, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void setDetectionInterval(double intervalInSeconds, String& outExceptionString);
    WebExtensionAPIEvent& onStateChanged();
private:
    void poll();
    std::unique_ptr<RunLoop::Timer> m_timer;
    RefPtr<WebExtensionAPIEvent> m_onStateChanged;
    Seconds m_detectionInterval { 60_s };
    String m_lastState { "active"_s };
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIIdle, idle);
#endif
