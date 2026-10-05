/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "WebExtensionContentWorldType.h"
#include "WebExtensionPortChannelIdentifier.h"
#include "WebPageProxyIdentifier.h"
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace WebKit {

// Count every port listener owned by a page so page teardown disconnects
// every endpoint, including several listeners on the same channel.
class WebExtensionPagePortMap {
public:
    using Channel = std::tuple<WebExtensionContentWorldType, WebExtensionContentWorldType, WebExtensionPortChannelIdentifier>;
    using PortCounts = HashCountedSet<Channel>;

    void add(WebPageProxyIdentifier page, Channel channel, unsigned count)
    {
        if (!count)
            return;
        m_pages.ensure(page, [] { return PortCounts { }; }).iterator->value.add(WTF::move(channel), count);
    }

    void remove(WebPageProxyIdentifier page, const Channel& channel)
    {
        auto entry = m_pages.find(page);
        if (entry == m_pages.end())
            return;
        entry->value.remove(channel);
        if (entry->value.isEmpty())
            m_pages.remove(entry);
    }

    Vector<WebPageProxyIdentifier> pages(WebExtensionContentWorldType world, WebExtensionPortChannelIdentifier identifier) const
    {
        Vector<WebPageProxyIdentifier> result;
        for (auto& page : m_pages) {
            for (auto& entry : page.value) {
                if (isEqual(std::get<0>(entry.key), world) && std::get<2>(entry.key) == identifier) {
                    result.append(page.key);
                    break;
                }
            }
        }
        return result;
    }

    bool contains(WebPageProxyIdentifier page) const { return m_pages.contains(page); }
    PortCounts take(WebPageProxyIdentifier page) { return m_pages.take(page); }
    void clear() { m_pages.clear(); }

private:
    HashMap<WebPageProxyIdentifier, PortCounts> m_pages;
};

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
