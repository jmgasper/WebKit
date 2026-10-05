#pragma once

#include "Cookie.h"

namespace WebCore {

// Only causes produced by the cookie backend are represented here. Curl does
// not yet evict cookies, so it must not fabricate an eviction notification.
enum class CookieChangeCause : uint8_t {
    Explicit,
    Overwrite,
    Expired,
    ExpiredOverwrite,
};

struct CookieChange {
    Cookie cookie;
    bool removed { false };
    CookieChangeCause cause { CookieChangeCause::Explicit };
};

} // namespace WebCore
