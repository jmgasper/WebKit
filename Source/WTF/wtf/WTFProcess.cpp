/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/WTFProcess.h>

#include <stdlib.h>
#include <stdio.h>

#if OS(UNIX)
#include <unistd.h>
#endif

#if OS(WINDOWS)
#include <windows.h>
#endif

#if OS(HAIKU)
#include <errno.h>
#include <fcntl.h>

// Haiku exits with _exit(), so instrumented training runs need an explicit
// flush. Keep ordinary builds independent of libgcov.
extern "C" void __gcov_dump() __attribute__((weak));

static void dumpProfileBeforeExit()
{
    const char* enabled = getenv("SUMMIT_GCOV_DUMP");
    if (!enabled || enabled[0] != '1' || enabled[1] || !__gcov_dump)
        return;
    // Haiku's libgcov merges into the .gcda files without locking them, so
    // processes that exit together overwrite each other's counters (and
    // sometimes leave a file half-written). Take turns.
    const char* lockPath = getenv("SUMMIT_GCOV_LOCK");
    int lockFile = open(lockPath && *lockPath ? lockPath : "/tmp/summit-gcov.lock", O_RDWR | O_CREAT, 0644);
    struct flock lock { };
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (lockFile >= 0) {
        while (fcntl(lockFile, F_SETLKW, &lock) == -1 && errno == EINTR) { }
    }
    __gcov_dump();
    if (lockFile >= 0)
        close(lockFile);
}

// Processes that leave through exit() (the UI process) would otherwise dump
// from libgcov's own unlocked destructor; once dumped, libgcov skips that one.
__attribute__((constructor)) static void dumpProfileAtExit()
{
    const char* enabled = getenv("SUMMIT_GCOV_DUMP");
    if (enabled && enabled[0] == '1' && !enabled[1] && __gcov_dump)
        atexit(dumpProfileBeforeExit);
}
#endif

namespace WTF {

void exitProcess(int status)
{
#if OS(WINDOWS)
    // Windows do not have "main thread" concept. So, shutdown of the main thread does not mean immediate process shutdown.
    // As a result, if there is running thread, it sometimes cause deadlock.
    //
    // > If one of the terminated threads in the process holds a lock and the DLL detach code in one of the loaded DLLs
    // > attempts to acquire the same lock, then calling ExitProcess results in a deadlock. In contrast, if a process
    // > terminates by calling TerminateProcess, the DLLs that the process is attached to are not notified of the process
    // > termination. Therefore, if you do not know the state of all threads in your process, it is better to call TerminateProcess
    // > than ExitProcess. Note that returning from the main function of an application results in a call to ExitProcess.
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess
    //
    // Always use TerminateProcess since framework does not know the status of the other threads.
    TerminateProcess(GetCurrentProcess(), status);

    // The code can reach here only when very buggy anti-virus software hooks are integrated into the system. To make it safe, in that case,
    // let the process crash explicitly.
    CRASH();
#elif OS(HAIKU)
    // libbe destroys its global handler-token table before other threads have
    // finished their pthread TLS destructors. Explicit process exit may leave
    // JavaScript agents alive; running those destructors against the destroyed
    // table can double-free BHandlers. Flush output, then terminate all threads
    // together without invoking C++ static teardown.
    fflush(nullptr);
    dumpProfileBeforeExit();
    _exit(status);
#else
    exit(status);
#endif
}

void terminateProcess(int status)
{
#if OS(WINDOWS)
    // On Windows, exitProcess and terminateProcess do the same thing due to its more complicated main thread handling.
    // See comment in exitProcess.
    exitProcess(status);
#elif OS(HAIKU)
    dumpProfileBeforeExit();
    _exit(status);
#else
    _exit(status);
#endif
}

}
