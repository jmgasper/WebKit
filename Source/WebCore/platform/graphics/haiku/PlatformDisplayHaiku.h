/*
 * Copyright (C) 2015 Igalia S.L
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

#pragma once

#if PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)

#include "PlatformDisplay.h"

namespace WebCore {

class PlatformDisplayHaiku final : public PlatformDisplay {
public:
    // Returns nullptr when EGL cannot be initialized. Unlike the other ports'
    // displays this never crashes: a missing or non-working EGL is an expected
    // configuration on Haiku and simply selects software rendering.
    //
    // The caller decides whether to install the result with
    // PlatformDisplay::setSharedDisplay(). That can be done at most once per
    // process, and PlatformDisplay::sharedDisplay() asserts when it was never
    // done, so probing code must use sharedDisplayIfExists().
    WEBCORE_EXPORT static std::unique_ptr<PlatformDisplayHaiku> create();

    virtual ~PlatformDisplayHaiku();

private:
    explicit PlatformDisplayHaiku(Ref<GLDisplay>&&);

    Type type() const final { return PlatformDisplay::Type::Haiku; }
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_PLATFORM_DISPLAY(PlatformDisplayHaiku, Haiku)

#endif // PLATFORM(HAIKU) && USE(TEXTURE_MAPPER)
