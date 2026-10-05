/*
 * Copyright (c) 2026 KunanyiOS contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <StorageDefs.h>
#include <String.h>
#include <algorithm>

namespace WebKit {

inline BString sanitizedDownloadFilename(const BString& suggested)
{
    BString name = suggested;
    int32 separator = std::max(name.FindLast('/'), name.FindLast('\\'));
    if (separator >= 0)
        name.Remove(0, separator + 1);
    for (int32 i = 0; i < name.Length(); ++i) {
        unsigned char byte = name.ByteAt(i);
        if (byte < 32 || byte == 127)
            name.SetByteAt(i, '_');
    }
    name.Trim();

    // Leave room for a collision counter, and never split a UTF-8 character.
    constexpr int32 maximumLength = B_FILE_NAME_LENGTH - 1 - 12;
    if (name.Length() > maximumLength) {
        int32 length = maximumLength;
        while (length && (static_cast<unsigned char>(name.ByteAt(length)) & 0xc0) == 0x80)
            --length;
        name.Truncate(length);
    }
    if (name.IsEmpty() || name == "." || name == "..")
        return BString("Download");
    return name;
}

inline BString downloadFilenameWithCounter(const BString& sanitizedName, uint32 counter)
{
    BString base = sanitizedName;
    BString extension;
    int32 extensionStart = base.FindLast('.');
    if (extensionStart > 0)
        base.MoveInto(extension, extensionStart, base.Length() - extensionStart);
    base << "-" << counter;
    return base.Append(extension);
}

} // namespace WebKit
