/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebKitInfo.h"

#include "WebKitBuildInfoHaiku.h"

const char* BWebKitVersion()
{
    return WEBKIT_ENGINE_VERSION_HAIKU;
}

const char* BWebKitPortVersion()
{
    return HAIKU_WEBKIT_VERSION "+summit.00991b6cdc59937da6076c69a22ae6a9460a4445";
}

const char* BWebKitSourceRevision()
{
    return "00991b6cdc59937da6076c69a22ae6a9460a4445";
}
