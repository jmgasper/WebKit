/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PlatformDisplayHaiku.h"

#if PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)

#include "GLDisplay.h"
#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#else
#include <EGL/egl.h>
#endif

namespace WebCore {

std::unique_ptr<PlatformDisplayHaiku> PlatformDisplayHaiku::create()
{
    // Mesa's Haiku EGL platform is the native platform of EGL_DEFAULT_DISPLAY.
    // GLDisplay::create() runs eglInitialize() and returns null when it fails,
    // which is what the stock Mesa 22 package does.
    auto glDisplay = GLDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));
    if (!glDisplay)
        return nullptr;

    return std::unique_ptr<PlatformDisplayHaiku>(new PlatformDisplayHaiku(glDisplay.releaseNonNull()));
}

PlatformDisplayHaiku::PlatformDisplayHaiku(Ref<GLDisplay>&& glDisplay)
    : PlatformDisplay(WTF::move(glDisplay))
{
#if ENABLE(WEBGL)
    // ANGLE's EGL back-end opens the same native display: platform 0 makes it
    // call eglGetDisplay(EGL_DEFAULT_DISPLAY) rather than eglGetPlatformDisplay,
    // which with a surfaceless platform hangs in zink (docs/workstation.md).
    m_anglePlatform = 0;
    m_angleNativeDisplay = EGL_DEFAULT_DISPLAY;
#endif
}

PlatformDisplayHaiku::~PlatformDisplayHaiku() = default;

} // namespace WebCore

#endif // PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)
