/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
#include "WebExtensionControllerConfiguration.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "StoragePathsHaiku.h"
#include <wtf/FileSystem.h>

namespace WebKit {

String WebExtensionControllerConfiguration::createStorageDirectoryPath(std::optional<WTF::UUID> identifier)
{
    auto identifierPath = identifier ? identifier->toString().convertToASCIIUppercase() : "Default"_s;
    auto directory = FileSystem::pathByAppendingComponent("WebExtensions"_s, identifierPath);
    // Use the same application profile root as native website data stores.
    // Persistent storage is created lazily by the controller's storage stores.
    return storageDirectoryPathHaiku(false, nullString(), directory, false);
}

String WebExtensionControllerConfiguration::createTemporaryStorageDirectoryPath()
{
    return FileSystem::createTemporaryDirectory();
}

Ref<WebExtensionControllerConfiguration> WebExtensionControllerConfiguration::copy() const
{
    // Creating a default configuration only computes a path. Copy the stored
    // state directly so temporary copies share their original directory and
    // nonpersistent copies share their original ephemeral website data store.
    auto result = createDefault();
    result->m_identifier = m_identifier;
    result->m_temporary = m_temporary;
    result->m_storageDirectory = m_storageDirectory;
    result->m_defaultWebsiteDataStore = m_defaultWebsiteDataStore;
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
