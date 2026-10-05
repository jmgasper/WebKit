/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#if PLATFORM(HAIKU)

#include <wtf/text/WTFString.h>

namespace WebKit {

// Staged package copies are as large as the installed extensions, so they belong
// on the profile's own volume rather than the system temporary directory, which
// may be on a small or full boot volume. Each browser run stages inside a
// directory named for its team, so a run that ends without cleaning up (a crash
// or a kill) leaves a directory that the next run can recognize and remove.
// An empty profile directory restores the system temporary directory.
void setWebExtensionStagingDirectoryHaiku(const String& profileDirectory);
String webExtensionStagingDirectoryHaiku();

// Where unpacked archives are kept between launches: <profile>/ExtensionPackageCache.
// Empty until a profile is set; preparation then unpacks into the staging directory.
String webExtensionPackageCacheDirectoryHaiku();
unsigned removeAbandonedWebExtensionStagingDirectoriesHaiku(const String& stagingRoot);

} // namespace WebKit

#endif // PLATFORM(HAIKU)
