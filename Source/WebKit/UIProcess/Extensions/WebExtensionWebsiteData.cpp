/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
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
#include "WebExtensionWebsiteData.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "WebExtensionMatchPattern.h"

namespace WebKit {

Vector<WebsiteDataRecord> staleWebExtensionWebsiteData(const Vector<WebsiteDataRecord>& records, const HashSet<String>& activeExtensionURLs)
{
    Vector<WebsiteDataRecord> staleRecords;
    for (auto& record : records) {
        WebsiteDataRecord staleRecord;
        for (auto& origin : record.origins) {
            if (WebExtensionMatchPattern::isWebExtensionURL(origin.toURL()) && !activeExtensionURLs.contains(origin.toURL().protocolHostAndPort().convertToASCIILowercase()))
                staleRecord.origins.add(origin);
        }
        if (!staleRecord.origins.isEmpty()) {
            staleRecord.types = record.types;
            staleRecords.append(WTF::move(staleRecord));
        }
    }
    return staleRecords;
}

OptionSet<WebsiteDataType> webExtensionWebsiteDataTypes()
{
    // Match the public and private website data types used by Cocoa cleanup.
    return {
        WebsiteDataType::DiskCache,
        WebsiteDataType::DOMCache,
        WebsiteDataType::MemoryCache,
        WebsiteDataType::Cookies,
        WebsiteDataType::SessionStorage,
        WebsiteDataType::LocalStorage,
        WebsiteDataType::IndexedDBDatabases,
        WebsiteDataType::ServiceWorkerRegistrations,
        WebsiteDataType::WebSQLDatabases,
        WebsiteDataType::FileSystem,
        WebsiteDataType::SearchFieldRecentSearches,
        WebsiteDataType::MediaKeys,
        WebsiteDataType::DeviceIdHashSalt,
        WebsiteDataType::HSTSCache,
        WebsiteDataType::ResourceLoadStatistics,
        WebsiteDataType::Credentials,
        WebsiteDataType::PrivateClickMeasurements,
        WebsiteDataType::EnhancedSecurityRecord,
        WebsiteDataType::IsolatedSiteRecord,
#if HAVE(ALTERNATIVE_SERVICE)
        WebsiteDataType::AlternativeServices,
#endif
#if ENABLE(SCREEN_TIME)
        WebsiteDataType::ScreenTime,
#endif
    };
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
