/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace WebKit {

// Native window threads may finish a request while the WebKit thread closes
// the page. Remove callbacks under the lock, then invoke them outside it. The
// bridge supplies callbacks that dispatch their WebKit completion onto main.
class JavaScriptDialogRequestsHaiku {
public:
    using Completion = std::function<void(bool, std::string)>;

    ~JavaScriptDialogRequestsHaiku() { cancelAll(true); }

    uint64_t add(Completion completion)
    {
        {
            std::lock_guard guard(m_mutex);
            if (!m_closed) {
                do { ++m_nextIdentifier; } while (!m_nextIdentifier || m_requests.contains(m_nextIdentifier));
                m_requests.emplace(m_nextIdentifier, std::move(completion));
                return m_nextIdentifier;
            }
        }
        completion(false, { });
        return 0;
    }

    bool hasPending() const
    {
        std::lock_guard guard(m_mutex);
        return !m_requests.empty();
    }

    bool contains(uint64_t identifier) const
    {
        std::lock_guard guard(m_mutex);
        return m_requests.contains(identifier);
    }

    bool complete(uint64_t identifier, bool accepted, std::string value = { })
    {
        Completion completion;
        {
            std::lock_guard guard(m_mutex);
            auto found = m_requests.find(identifier);
            if (found == m_requests.end())
                return false;
            completion = std::move(found->second);
            m_requests.erase(found);
        }
        completion(accepted, std::move(value));
        return true;
    }

    void cancelAll(bool close = false)
    {
        std::map<uint64_t, Completion> requests;
        {
            std::lock_guard guard(m_mutex);
            m_closed |= close;
            requests.swap(m_requests);
        }
        for (auto& [identifier, completion] : requests)
            completion(false, { });
    }

private:
    mutable std::mutex m_mutex;
    uint64_t m_nextIdentifier { 0 };
    bool m_closed { false };
    std::map<uint64_t, Completion> m_requests;
};

} // namespace WebKit
