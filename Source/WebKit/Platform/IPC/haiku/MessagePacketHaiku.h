/*
 * Copyright (C) 2026 KunanyiOS contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Attachment.h"
#include <WebCore/SharedMemory.h>
#include <array>
#include <utility>
#include <sys/socket.h>
#include <wtf/Vector.h>

namespace IPC {

inline constexpr size_t messageMaxSizeHaiku = 4096;
// Haiku caps the userspace control buffer at 1024 bytes and each kernel
// ancillary record at 32 descriptor pointers. Send larger lists in groups.
inline constexpr size_t descriptorGroupSizeHaiku = 32;
inline constexpr size_t controlBufferSizeHaiku = 1024;
inline constexpr size_t attachmentMaxAmountHaiku = controlBufferSizeHaiku
    / CMSG_SPACE(descriptorGroupSizeHaiku * sizeof(int)) * descriptorGroupSizeHaiku;

class FileDescriptorMessageHaiku {
public:
    bool setDescriptors(std::span<const int>);
    void attachTo(msghdr&);
private:
    alignas(cmsghdr) std::array<uint8_t, controlBufferSizeHaiku> m_bytes { };
    size_t m_size { 0 };
};

// This byte is copied across the socket; a C++ bool cannot safely represent
// every possible input byte from a peer.
struct AttachmentInfoHaiku {
    uint8_t isNull { 0 };
    bool isValid() const { return isNull <= 1; }
};
static_assert(sizeof(AttachmentInfoHaiku) == 1);

// Owns one SOCK_SEQPACKET message and every descriptor received with it.
// Invalid packets cannot leave descriptors behind for the next message.
class MessagePacketHaiku {
public:
    enum class Result { Message, WouldBlock, Closed, Error };
    Result receive(int socket);
    std::span<const uint8_t> body() const;
    Vector<Attachment> takeAttachments() { return std::exchange(m_attachments, { }); }

private:
    bool decode(size_t bytes, Vector<UnixFileDescriptor>&&);
    std::array<uint8_t, messageMaxSizeHaiku> m_bytes;
    size_t m_bodyOffset { 0 };
    size_t m_bodySize { 0 };
    Vector<Attachment> m_attachments;
    RefPtr<WebCore::SharedMemory> m_sharedBody;
};

} // namespace IPC
