/*
 *  Copyright (C) 2019 Haiku, Inc.
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
#include "ProcessLauncher.h"

#include "StartupTraceHaiku.h"
#include "HaikuProcess.h"
#include "IPCUtilities.h"
#include "ProcessExecutablePath.h"
#include <OS.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <signal.h>
#include <image.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/CString.h>

using namespace WebCore;

namespace WebKit {

// Haiku starts programs with 256 descriptors. A web process holds two per
// frame buffer besides its IPC sockets and font files, and the network
// process allows 256 connections, so both run out in a long session. Raise
// the limit before the first child starts: they inherit it, and so does the
// UI process, which maps every tab's frame buffers.
static void raiseFileDescriptorLimit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        struct rlimit limits { };
        if (getrlimit(RLIMIT_NOFILE, &limits))
            return;
        constexpr rlim_t wanted = 4096;
        rlim_t target = limits.rlim_max == RLIM_INFINITY ? wanted : std::min(limits.rlim_max, wanted);
        if (limits.rlim_cur != RLIM_INFINITY && limits.rlim_cur < target) {
            limits.rlim_cur = target;
            setrlimit(RLIMIT_NOFILE, &limits);
        }
    });
}

// A browser killed while load_image() starts a helper (a third of a second
// for a web process, up to 0.6 s on a Raspberry Pi) leaves that helper for
// good: the new team waits with its main thread suspended until its parent
// resumes it, and once the parent is gone nothing will. Each one holds its
// libraries, about 10 MiB. The first launch removes such helpers of this
// engine: the same executable file, the parent gone (the kernel team adopts
// orphans), and every thread suspended. A helper whose parent died after
// resuming it ends by itself when its connection closes.
static void removeAbandonedHelpers()
{
    struct stat executables[2] { };
    unsigned count = 0;
    for (const auto& path : { executablePathOfWebProcess(), executablePathOfNetworkProcess() }) {
        auto nativePath = path.utf8();
        if (!path.isEmpty() && !stat(nativePath.legacyCStringPointer(), &executables[count]))
            ++count;
    }
    if (!count)
        return;
    int32 cookie = 0;
    team_info team;
    while (get_next_team_info(&cookie, &team) == B_OK) {
        team_info parent;
        if (team.team == getpid() || (team.parent > B_SYSTEM_TEAM && get_team_info(team.parent, &parent) == B_OK))
            continue;
        int32 imageCookie = 0;
        image_info image;
        bool helper = false;
        while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
            if (image.type != B_APP_IMAGE)
                continue;
            struct stat info;
            if (!stat(image.name, &info)) {
                for (unsigned i = 0; i < count; ++i)
                    helper |= info.st_dev == executables[i].st_dev && info.st_ino == executables[i].st_ino;
            }
            break;
        }
        if (!helper)
            continue;
        int32 threadCookie = 0;
        thread_info thread;
        unsigned threads = 0;
        bool suspended = true;
        while (get_next_thread_info(team.team, &threadCookie, &thread) == B_OK) {
            ++threads;
            suspended &= thread.state == B_THREAD_SUSPENDED;
        }
        if (threads && suspended && kill_team(team.team) == B_OK)
            std::fprintf(stderr, "Summit: removed %s (team %d), left suspended by a browser that died while starting it\n",
                image.name, static_cast<int>(team.team));
    }
}

// The directory above the lib/ this library was loaded from: the bundle or
// the summit_webkit package.
static std::string engineDirectory()
{
    int32 cookie = 0;
    image_info image;
    auto self = reinterpret_cast<addr_t>(&engineDirectory);
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        auto text = reinterpret_cast<addr_t>(image.text);
        if (self < text || self - text >= static_cast<addr_t>(image.text_size))
            continue;
        std::string path = image.name;
        auto slash = path.rfind('/');
        if (slash == std::string::npos || slash == 0)
            return { };
        path.resize(slash);
        slash = path.rfind('/');
        return slash == std::string::npos || slash == 0 ? std::string { } : path.substr(0, slash);
    }
    return { };
}

// Stock Haiku's Mesa has no EGL, so the summit_webkit package carries its own
// (engine/mesa/lib and engine/mesa/egl_vendor.d). Point the web and network
// processes at it unless the embedder chose a glvnd vendor itself, as Summit's
// launcher does for the GPU driver. A bundle without mesa/ changes nothing.
static const std::vector<std::string>& childEnvironmentOverrides()
{
    static NeverDestroyed<std::vector<std::string>> overrides = [] {
        std::vector<std::string> result;
        if (getenv("__EGL_VENDOR_LIBRARY_FILENAMES") || getenv("__EGL_VENDOR_LIBRARY_DIRS"))
            return result;
        auto engine = engineDirectory();
        if (engine.empty())
            return result;
        auto vendor = engine + "/mesa/egl_vendor.d/50_mesa.json";
        struct stat info;
        if (stat(vendor.c_str(), &info) || !S_ISREG(info.st_mode))
            return result;
        const char* path = getenv("LIBRARY_PATH");
        result.push_back("__EGL_VENDOR_LIBRARY_FILENAMES=" + vendor);
        result.push_back("LIBRARY_PATH=" + engine + "/mesa/lib:" + (path && *path ? path
            : "%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib"));
        return result;
    }();
    return overrides.get();
}

void ProcessLauncher::launchProcess()
{
    raiseFileDescriptorLimit();
    static std::once_flag abandonedHelpersOnce;
    std::call_once(abandonedHelpersOnce, [] {
        Thread::create("WebKit helper cleanup"_s, [] {
            removeAbandonedHelpers();
        })->detach();
    });
    String executable;
    switch (m_launchOptions.processType) {
    case ProcessType::Web:
        executable = executablePathOfWebProcess();
        break;
    case ProcessType::Network:
        executable = executablePathOfNetworkProcess();
        break;
#if ENABLE(GPU_PROCESS)
    case ProcessType::GPU:
        executable = executablePathOfGPUProcess();
        break;
#endif
    default:
        break;
    }
    auto sockets = IPC::createPlatformConnection(SOCK_SEQPACKET);
    std::vector<std::string> arguments;
    if (m_client && m_client->shouldConfigureJSCForTesting())
        arguments.emplace_back("--configure-jsc-for-testing");
    // load_image() returns once the new team has loaded its libraries, a
    // third of a second for a web process: not on the main thread, and not
    // one launch after another.
    Thread::create("WebKit process launcher"_s, [protectedThis = Ref { *this }, executable = executable.utf8(), processIdentifier = m_launchOptions.processIdentifier.toUInt64(),
        failForTesting = m_launchOptions.shouldMakeProcessLaunchFailForTesting, client = WTF::move(sockets.client), server = WTF::move(sockets.server),
        arguments = WTF::move(arguments)] mutable {
        const char* name = strrchr(executable.legacyCStringPointer(), '/');
        name = name ? name + 1 : executable.legacyCStringPointer();
        SUMMIT_STARTUP_TRACE("launching %s", name);
        auto result = failForTesting
            ? HaikuProcessResult { 0, ENOENT }
            : spawnHaikuProcess(executable.legacyCStringPointer(), processIdentifier, client.value(), arguments, childEnvironmentOverrides());
        SUMMIT_STARTUP_TRACE("%s team %d has loaded its libraries", name, static_cast<int>(result.process));
        client = { };
        if (result.error) {
            RunLoop::mainSingleton().dispatch([protectedThis = WTF::move(protectedThis)] {
                protectedThis->didFinishLaunchingProcess(0, { });
            });
            return;
        }
        Thread::create("WebKit process reaper"_s, [process = result.process] {
            int status;
            while (waitpid(process, &status, 0) < 0 && errno == EINTR) { }
        })->detach();
        RunLoop::mainSingleton().dispatch([protectedThis = WTF::move(protectedThis), process = result.process, server = WTF::move(server)] mutable {
            if (!protectedThis->m_client)
                kill(process, SIGKILL);
            protectedThis->didFinishLaunchingProcess(process, IPC::Connection::Identifier { WTF::move(server) });
        });
    })->detach();
}

void ProcessLauncher::terminateProcess(const String&)
{
    if (m_processID)
        kill(m_processID, SIGKILL);
}

void ProcessLauncher::platformInvalidate()
{
    m_processID = 0;
}

} // namespace WebKit
