/*
 * Copyright (c) 2026 KunanyiOS contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <OS.h>
#include <fcntl.h>
#include <image.h>
#include <signal.h>
#include <sys/wait.h>
#include <string>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace WebKit {

struct HaikuProcessResult {
    pid_t process { 0 };
    int error { 0 };
};

// Match AuxiliaryProcessMainCommon's process-identifier/socket argument pair.
// File descriptors owned by the caller must have FD_CLOEXEC unless deliberately
// inherited; the IPC endpoint alone is made inheritable for the child here.
//
// The child is started with load_image(), a new team with an address space of
// its own, not with fork() and exec. A fork copies the whole browser process
// first, and when the browser's window is a direct window that copy includes
// its mapping of the screen's frame buffer, which the kernel cannot copy on
// write: on the X399 the first web process launched after the window was
// connected (B_DIRECT_DEVICE_PIXELS) hung the machine every time. Copying the
// browser's address space also cost every launch time for nothing.
inline HaikuProcessResult spawnHaikuProcess(const char* executable, uint64_t processIdentifier,
    int connection, const std::vector<std::string>& extraArguments = { }, const std::vector<std::string>& environmentOverrides = { })
{
    if (!executable || executable[0] != '/' || !processIdentifier)
        return { 0, EINVAL };

    // A team started by load_image() inherits every descriptor without
    // FD_CLOEXEC; the endpoint's duplicate is the only such one created here.
    // Launches run at once (load_image() returns only when the child's
    // libraries are loaded, a third of a second for a web process), so a
    // child may inherit another's endpoint too: each closes all it inherited
    // but its own first thing (AuxiliaryProcessMainCommon::parseCommandLine).
    int duplicate = fcntl(connection, F_DUPFD, 4);
    if (duplicate < 0)
        return { 0, errno };

    std::vector<std::string> arguments { executable, std::to_string(processIdentifier), std::to_string(duplicate) };
    arguments.insert(arguments.end(), extraArguments.begin(), extraArguments.end());
    std::vector<const char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    // The child's environment: this one, with NAME=VALUE overrides replacing
    // variables of the same name.
    std::vector<const char*> envp;
    for (char** variable = environ; variable && *variable; ++variable) {
        const char* equals = strchr(*variable, '=');
        size_t length = equals ? equals - *variable + 1 : strlen(*variable);
        bool overridden = false;
        for (auto& entry : environmentOverrides)
            overridden |= entry.size() >= length && !entry.compare(0, length, *variable, length);
        if (!overridden)
            envp.push_back(*variable);
    }
    for (auto& entry : environmentOverrides)
        envp.push_back(entry.c_str());
    envp.push_back(nullptr);

    thread_id thread = load_image(static_cast<int32>(arguments.size()), argv.data(), envp.data());
    int error = 0;
    if (thread < 0)
        error = thread == B_ENTRY_NOT_FOUND ? ENOENT : EIO;
    else if (resume_thread(thread) != B_OK) {
        kill_thread(thread);
        error = EIO;
    }
    close(duplicate);
    // The main thread's identifier is the team's, and the team is our child.
    return { error ? 0 : static_cast<pid_t>(thread), error };
}

} // namespace WebKit
