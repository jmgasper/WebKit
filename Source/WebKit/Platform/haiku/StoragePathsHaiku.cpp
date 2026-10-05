/*
 * Copyright (C) 2026 KunanyiOS contributors.
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

#include "config.h"
#include "StoragePathsHaiku.h"

#include <Application.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <wtf/FileSystem.h>
#include <wtf/text/StringView.h>

namespace WebKit {

String storageDirectoryPathHaiku(bool cache, const String& baseDirectory, const String& directoryName, bool create)
{
    if (directoryName.contains('\0') || baseDirectory.contains('\0') || directoryName.startsWith('/'))
        return { };
    for (auto component : StringView(directoryName).split('/')) {
        if (component == ".."_s)
            return { };
    }

    String base = baseDirectory;
    if (base.isNull()) {
        BPath nativePath;
        if (!be_app || !be_app->Signature()
            || find_directory(cache ? B_USER_CACHE_DIRECTORY : B_USER_SETTINGS_DIRECTORY, &nativePath) != B_OK)
            return { };
        auto application = FileSystem::encodeForFileName(String::fromUTF8(be_app->Signature()));
        base = FileSystem::pathByAppendingComponent(String::fromUTF8(nativePath.Path()), "WebKit"_s);
        base = FileSystem::pathByAppendingComponent(base, application);
    }
    // An explicitly empty profile base must never become a path relative to
    // the current working directory.
    if (base.isEmpty() || !base.startsWith('/'))
        return { };
    auto result = directoryName.isEmpty() ? base : FileSystem::pathByAppendingComponent(base, directoryName);
    if (create && create_directory(FileSystem::fileSystemRepresentation(result).legacyCStringPointer(), 0700) != B_OK)
        return { };
    return result;
}

} // namespace WebKit
