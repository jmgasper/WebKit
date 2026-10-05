/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <SupportDefs.h>

// A request contains identifier (opaque string), extension_identifier and
// extension_name (strings), private_browsing (bool), deadline (int64, system_time
// microseconds), and repeated permission/origin strings. All capabilities in
// this one request require one decision. Cancellation contains identifier only.
// Hosts must dismiss cancelled/expired requests and ignore late UI responses.
enum : uint32 {
    B_WEBKIT_EXTENSION_PERMISSION_REQUESTED = 'wepr',
    B_WEBKIT_EXTENSION_PERMISSION_CANCELLED = 'wepc'
};
