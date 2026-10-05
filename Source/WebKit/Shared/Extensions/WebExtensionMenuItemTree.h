/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <optional>
#include <wtf/Function.h>
#include <wtf/text/WTFString.h>
namespace WebKit {
// Walk the proposed parent's ancestors before mutating the tree. A corrupt
// existing cycle also fails rather than looping indefinitely.
bool canReparentWebExtensionMenuItem(const String& item, const String& proposedParent, const Function<std::optional<String>(const String&)>& parentForItem);
}
#endif
