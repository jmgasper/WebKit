/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <WebKit/WKBase.h>

// These strings describe the loaded library. They do not require WebKit initialization.
WK_EXPORT const char* BWebKitVersion();
WK_EXPORT const char* BWebKitPortVersion();
WK_EXPORT const char* BWebKitSourceRevision();
