/*
 * Copyright (C) 2012 Samsung Electronics
 * Copyright (C) 2014 Haiku, inc.
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
#include "ProcessExecutablePath.h"

#include <image.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>

namespace WebKit {

static bool isExecutable(const String& path)
{
    auto nativePath = path.utf8();
    const char* filename = nativePath.legacyCStringPointer();
    struct stat info;
    return filename && filename[0] == '/' && !stat(filename, &info)
        && S_ISREG(info.st_mode) && !access(filename, X_OK);
}

static String findProcessPath(const char* processName)
{
    auto name = String::fromUTF8(processName);
    if (const char* directory = getenv("WEBKIT_EXEC_PATH")) {
        auto path = FileSystem::pathByAppendingComponent(String::fromUTF8(directory), name);
        if (isExecutable(path))
            return path;
    }

    // Haiku exposes loaded images directly; it has no /proc/self/exe link.
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (image.type != B_APP_IMAGE)
            continue;
        auto directory = FileSystem::parentPath(String::fromUTF8(image.name));
        auto path = FileSystem::pathByAppendingComponent(directory, name);
        if (isExecutable(path))
            return path;
        break;
    }

    // An engine installed on its own (the summit_webkit package) keeps its
    // processes in the directory above lib/, where this library lives.
    cookie = 0;
    auto self = reinterpret_cast<addr_t>(&findProcessPath);
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        auto text = reinterpret_cast<addr_t>(image.text);
        if (self < text || self - text >= static_cast<addr_t>(image.text_size))
            continue;
        auto directory = FileSystem::parentPath(String::fromUTF8(image.name));
        for (auto candidate : { FileSystem::parentPath(directory), directory }) {
            auto path = FileSystem::pathByAppendingComponent(candidate, name);
            if (isExecutable(path))
                return path;
        }
        break;
    }
    auto path = FileSystem::pathByAppendingComponent(String::fromUTF8(LIBEXECDIR), name);
    return isExecutable(path) ? path : String();
}

String executablePathOfWebProcess()
{
    static NeverDestroyed<String> path(findProcessPath(WEBPROCESSNAME));
    return path.get();
}

String executablePathOfNetworkProcess()
{
    static NeverDestroyed<String> path(findProcessPath(NETWORKPROCESSNAME));
    return path.get();
}

#if ENABLE(GPU_PROCESS)
String executablePathOfGPUProcess()
{
    static NeverDestroyed<String> path(findProcessPath("GPUProcess"));
    return path.get();
}
#endif

} // namespace WebKit
