/*
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2011,2017 Igalia S.L.
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

#include <wtf/StdLibExtras.h>

namespace IPC {

class MessageInfo {
public:
    MessageInfo()
    {
        // The entire MessageInfo is passed to write(), so we have to zero our
        // padding bytes to avoid writing uninitialized memory.
        zeroBytes(*this);
    }

#if OS(ANDROID)
    MessageInfo(size_t bodySize, size_t initialAttachmentCount, size_t hardwareBufferCount)
        : MessageInfo()
    {
        m_bodySize = bodySize;
        m_attachmentCount = initialAttachmentCount;
        m_hardwareBufferCount = hardwareBufferCount;
    }

    size_t hardwareBufferCount() const { return m_hardwareBufferCount; }
#else
    MessageInfo(size_t bodySize, size_t initialAttachmentCount)
        : MessageInfo()
    {
        m_bodySize = bodySize;
        m_attachmentCount = initialAttachmentCount;
    }
#endif

    MessageInfo(const MessageInfo& info)
        : MessageInfo()
    {
        *this = info;
    }

    MessageInfo& operator=(const MessageInfo&) = default;

    void setBodyOutOfLine()
    {
        ASSERT(!isBodyOutOfLine());

        m_isBodyOutOfLine = true;
        m_attachmentCount++;
    }

    bool isValid() const { return m_isBodyOutOfLine <= 1; }
    bool isBodyOutOfLine() const { return m_isBodyOutOfLine; }
    size_t bodySize() const { return m_bodySize; }
    size_t attachmentCount() const { return m_attachmentCount; }

private:
    // The MessageInfo will be copied using memcpy, so all members must be trivially copyable.
    size_t m_bodySize;
    size_t m_attachmentCount;
#if OS(ANDROID)
    size_t m_hardwareBufferCount;
#endif
    uint8_t m_isBodyOutOfLine;
};

} // namespace IPC
