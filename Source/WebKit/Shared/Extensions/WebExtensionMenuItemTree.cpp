/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionMenuItemTree.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <wtf/HashSet.h>
#include <wtf/text/StringHash.h>
namespace WebKit {
bool canReparentWebExtensionMenuItem(const String& item, const String& proposedParent, const Function<std::optional<String>(const String&)>& parentForItem)
{
    HashSet<String> visited;
    std::optional<String> ancestor = proposedParent;
    while (ancestor) {
        if (*ancestor == item || !visited.add(*ancestor).isNewEntry)
            return false;
        ancestor = parentForItem(*ancestor);
    }
    return true;
}
}
#endif
