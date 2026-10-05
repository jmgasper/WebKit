/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionCookieStoreIdentifier.h"
#if ENABLE(WK_WEB_EXTENSIONS)
#include <wtf/text/MakeString.h>
namespace WebKit {
std::optional<PAL::SessionID> parseWebExtensionCookieStoreIdentifier(const String& value)
{
    constexpr auto persistentPrefix = "persistent-"_s;
    constexpr auto ephemeralPrefix = "ephemeral-"_s;
    bool ephemeral = value.startsWith(ephemeralPrefix);
    if (!ephemeral && !value.startsWith(persistentPrefix))
        return std::nullopt;
    unsigned offset = ephemeral ? ephemeralPrefix.length() : persistentPrefix.length();
    if (offset == value.length())
        return std::nullopt;
    constexpr uint64_t maximum = PAL::SessionID::EphemeralSessionMask - 1;
    uint64_t number = 0;
    for (; offset < value.length(); ++offset) {
        auto character = value[offset];
        if (character < '0' || character > '9')
            return std::nullopt;
        unsigned digit = character - '0';
        if (number > (maximum - digit) / 10)
            return std::nullopt;
        number = number * 10 + digit;
    }
    PAL::SessionID result(number | (ephemeral ? uint64_t { PAL::SessionID::EphemeralSessionMask } : 0));
    return result.isValid() ? std::optional(result) : std::nullopt;
}
String webExtensionCookieStoreIdentifier(PAL::SessionID session)
{
    if (!session.isValid())
        return nullString();
    return makeString(session.isEphemeral() ? "ephemeral-"_s : "persistent-"_s,
        session.toUInt64() & ~PAL::SessionID::EphemeralSessionMask);
}
}
#endif
