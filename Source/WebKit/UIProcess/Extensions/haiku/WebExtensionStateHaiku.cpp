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

#include "config.h"
#include "WebExtensionStateHaiku.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <wtf/FileSystem.h>
#include <wtf/FileHandle.h>
#include <wtf/Scope.h>

namespace WebKit {

std::expected<Ref<JSON::Object>, WebExtensionStateErrorHaiku> readWebExtensionStateHaiku(const String& path)
{
    if (!path.startsWith('/') || path.contains(static_cast<UChar>(0)))
        return makeUnexpected(WebExtensionStateErrorHaiku::InvalidPath);
    struct stat status;
    if (::stat(FileSystem::fileSystemRepresentation(path).legacyCStringPointer(), &status)) {
        if (errno == ENOENT)
            return JSON::Object::create();
        return makeUnexpected(WebExtensionStateErrorHaiku::ReadFailed);
    }
    if (!S_ISREG(status.st_mode))
        return makeUnexpected(WebExtensionStateErrorHaiku::ReadFailed);
    if (!status.st_size)
        return makeUnexpected(WebExtensionStateErrorHaiku::InvalidJSON);
    auto data = FileSystem::readEntireFile(path);
    if (!data)
        return makeUnexpected(WebExtensionStateErrorHaiku::ReadFailed);
    if (RefPtr value = JSON::Value::parseJSON(String::fromUTF8(data->span()))) {
        if (RefPtr state = value->asObject())
            return state.releaseNonNull();
    }
    return makeUnexpected(WebExtensionStateErrorHaiku::InvalidJSON);
}

std::expected<void, WebExtensionStateErrorHaiku> writeWebExtensionStateHaiku(const String& path, const JSON::Object& state)
{
    if (!path.startsWith('/') || path.contains(static_cast<UChar>(0)))
        return makeUnexpected(WebExtensionStateErrorHaiku::InvalidPath);
    auto [temporaryPath, handle] = FileSystem::openTemporaryFile("State-"_s, ".json"_s, FileSystem::parentPath(path));
    if (!handle)
        return makeUnexpected(WebExtensionStateErrorHaiku::TemporaryFileFailed);
    auto cleanup = makeScopeExit([&] { FileSystem::deleteFile(temporaryPath); });
    auto bytes = state.toJSONString().utf8();
    if (handle.write(byteCast<uint8_t>(bytes.span())) != bytes.length() || !handle.flush())
        return makeUnexpected(WebExtensionStateErrorHaiku::WriteFailed);
    handle = { };

    // An atomic rename in the same directory preserves the old state if
    // replacement fails. A fallback copy could instead truncate that state.
    if (::rename(FileSystem::fileSystemRepresentation(temporaryPath).legacyCStringPointer(), FileSystem::fileSystemRepresentation(path).legacyCStringPointer()))
        return makeUnexpected(WebExtensionStateErrorHaiku::ReplaceFailed);
    cleanup.release();
    return { };
}

HashSet<String> enabledWebExtensionRulesetsHaiku(const HashMap<String, bool>& manifestDefaults, const JSON::Value* savedState)
{
    auto saved = savedState ? savedState->asObject() : nullptr;
    HashSet<String> enabledRulesets;
    for (auto& entry : manifestDefaults) {
        auto savedValue = saved ? saved->getBoolean(entry.key) : std::nullopt;
        if (savedValue.value_or(entry.value))
            enabledRulesets.addVoid(entry.key);
    }
    return enabledRulesets;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
