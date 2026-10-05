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

#include <cstdlib>
#include <memory>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/URL.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

inline URL fileURLForWebExtensionPathHaiku(const String& path)
{
    if (path.isEmpty() || !path.startsWith('/') || path.contains('\0'))
        return { };

    // URL::fileURLWithFileSystemPath preserves existing percent escapes, and
    // the URL parser treats literal backslashes as separators. Native POSIX
    // filenames need those bytes escaped before entering the URL parser.
    constexpr char hex[] = "0123456789ABCDEF";
    StringBuilder builder;
    builder.append("file://"_s);
    auto pathBytes = path.utf8();
    if (pathBytes.isNull())
        return { };
    for (auto character : pathBytes.span()) {
        auto byte = static_cast<uint8_t>(character);
        if (isASCIIAlphanumeric(byte) || byte == '/' || byte == '-' || byte == '.' || byte == '_' || byte == '~')
            builder.append(static_cast<char>(byte));
        else
            builder.append('%', hex[byte >> 4], hex[byte & 15]);
    }
    return URL { builder.toString() };
}

inline String canonicalWebExtensionPathHaiku(const String& path)
{
    if (path.isEmpty() || !path.startsWith('/') || path.contains('\0'))
        return { };

    // FileSystem::realPath returns its input when canonicalization fails.
    // Resource boundaries instead need an explicit failure in that case.
    auto bytes = FileSystem::fileSystemRepresentation(path);
    if (bytes.isNull())
        return { };
    std::unique_ptr<char, decltype(&std::free)> canonical(realpath(bytes.legacyCStringPointer(), nullptr), &std::free);
    return canonical ? String::fromUTF8(canonical.get()) : String { };
}

inline URL resolveWebExtensionResourceURLHaiku(const URL& baseURL, const String& originalPath)
{
    if (!baseURL.isValid() || !baseURL.protocolIsFile() || !baseURL.host().isEmpty()
        || originalPath.isEmpty() || originalPath.contains('\0'))
        return { };

    auto path = originalPath.startsWith('/') ? originalPath.substring(1) : originalPath;
    if (path.isEmpty())
        return { };

    URL candidate { baseURL, path };
    if (!candidate.isValid() || !candidate.protocolIsFile() || !candidate.host().isEmpty())
        return { };

    auto basePath = canonicalWebExtensionPathHaiku(baseURL.fileSystemPath());
    auto resourcePath = canonicalWebExtensionPathHaiku(candidate.fileSystemPath());
    if (basePath.isEmpty() || resourcePath.isEmpty() || !FileSystem::isAncestor(basePath, resourcePath))
        return { };

    // Read the path we checked, rather than retaining a symlink spelling.
    return fileURLForWebExtensionPathHaiku(resourcePath);
}

} // namespace WebKit

#endif // PLATFORM(HAIKU)
