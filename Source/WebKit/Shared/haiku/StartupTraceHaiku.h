/*
 * Copyright (C) 2026 Summit contributors
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if PLATFORM(HAIKU)

#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace WebKit {

// SUMMIT_STARTUP_TRACE=1 prints one line per start-up milestone of the
// browser's processes (launch, libraries loaded, EGL, page, compositor,
// first frame), stamped with system_time() in seconds: the clock Summit's
// own timing lines use, so one log orders all the processes' lines.
inline bool startupTraceEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_STARTUP_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

// SUMMIT_QUIT_TRACE=1 does the same for closing a page: the close request,
// the web process's answer, time-outs and the commit.
inline bool quitTraceEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_QUIT_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

} // namespace WebKit

#define SUMMIT_STARTUP_TRACE(format, ...) do { \
    if (WebKit::startupTraceEnabledHaiku()) \
        std::fprintf(stderr, "Summit startup %.3f pid=%d " format "\n", system_time() / 1e6, static_cast<int>(getpid()), ##__VA_ARGS__); \
} while (0)

#define SUMMIT_QUIT_TRACE(format, ...) do { \
    if (WebKit::quitTraceEnabledHaiku()) \
        std::fprintf(stderr, "Summit quit %.3f pid=%d " format "\n", system_time() / 1e6, static_cast<int>(getpid()), ##__VA_ARGS__); \
} while (0)

#endif // PLATFORM(HAIKU)
