/*
 * Copyright (C) 2025 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "WebExtension.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include <wtf/glib/GRefPtr.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

WebExtension::WebExtension(GFile* resourcesFile, RefPtr<API::Error>& outError)
    : m_manifestJSON(JSON::Value::null())
{
    RELEASE_ASSERT(resourcesFile);

    outError = nullptr;

    GUniquePtr<char> baseURL(g_file_get_uri(resourcesFile));
    m_resourceBaseURL = URL { makeString(String::fromUTF8(baseURL.get()), "/"_s) };

    if (m_resourceBaseURL.isValid()) {
        auto isDirectory = g_file_query_file_type(resourcesFile, G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY;

        if (!isDirectory) {
            outError = createError(Error::Unknown);
            return;
        }
    }

    if (!manifestParsedSuccessfully()) {
        ASSERT(!m_errors.isEmpty());
        outError = m_errors.last().ptr();
    }
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
