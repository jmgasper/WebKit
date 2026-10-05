/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionStagingDirectoryHaiku.h"

#if PLATFORM(HAIKU)

#include <OS.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebKit {

static Lock& stagingDirectoryLock()
{
    static NeverDestroyed<Lock> lock;
    return lock.get();
}

static String& stagingDirectory()
{
    static NeverDestroyed<String> directory;
    return directory.get();
}

static String& packageCacheDirectory()
{
    static NeverDestroyed<String> directory;
    return directory.get();
}

String webExtensionPackageCacheDirectoryHaiku()
{
    Locker locker { stagingDirectoryLock() };
    return packageCacheDirectory().isolatedCopy();
}

String webExtensionStagingDirectoryHaiku()
{
    Locker locker { stagingDirectoryLock() };
    return stagingDirectory().isolatedCopy();
}

unsigned removeAbandonedWebExtensionStagingDirectoriesHaiku(const String& stagingRoot)
{
    if (stagingRoot.isEmpty())
        return 0;
    unsigned removed = 0;
    for (auto& name : FileSystem::listDirectory(stagingRoot)) {
        auto team = parseInteger<int32_t>(name);
        // Anything not named for a team, and every team that has exited, is
        // left over from an earlier run and can no longer be in use.
        if (team && *team > 0) {
            team_info info { };
            if (get_team_info(*team, &info) == B_OK)
                continue;
        }
        if (FileSystem::deleteNonEmptyDirectory(FileSystem::pathByAppendingComponent(stagingRoot, name)))
            ++removed;
    }
    return removed;
}

void setWebExtensionStagingDirectoryHaiku(const String& profileDirectory)
{
    if (profileDirectory.isEmpty()) {
        Locker locker { stagingDirectoryLock() };
        stagingDirectory() = { };
        packageCacheDirectory() = { };
        return;
    }
    auto root = FileSystem::pathByAppendingComponent(profileDirectory, "ExtensionStaging"_s);
    auto directory = FileSystem::pathByAppendingComponent(root, String::number(getpid()));
    // A staging directory that cannot be created leaves the system temporary
    // directory in use, which is where these copies were made before.
    if (!FileSystem::makeAllDirectories(directory))
        return;
    removeAbandonedWebExtensionStagingDirectoriesHaiku(root);
    Locker locker { stagingDirectoryLock() };
    stagingDirectory() = directory;
    packageCacheDirectory() = FileSystem::pathByAppendingComponent(profileDirectory, "ExtensionPackageCache"_s);
}

} // namespace WebKit

#endif // PLATFORM(HAIKU)
