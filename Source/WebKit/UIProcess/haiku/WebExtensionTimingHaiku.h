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

#pragma once

#if PLATFORM(HAIKU)

#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace WebKit {

// SUMMIT_EXTENSION_TIMING=1 prints one line per extension milestone (package
// preparation, load, background start, popup presentation) stamped with
// system_time() in seconds, the clock Summit's own timing lines use, so the
// browser's and the engine's lines interleave on one scale.
inline bool webExtensionTimingEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_EXTENSION_TIMING");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

} // namespace WebKit

#define SUMMIT_EXTENSION_TIMING(format, ...) do { \
    if (WebKit::webExtensionTimingEnabledHaiku()) \
        std::fprintf(stderr, "Summit extension timing %.3f " format "\n", system_time() / 1e6, ##__VA_ARGS__); \
} while (0)

#endif // PLATFORM(HAIKU)
