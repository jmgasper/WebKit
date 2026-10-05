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

#include "config.h"
#include "MessagePacketHaiku.h"

#include "MessageInfo.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace IPC {

bool FileDescriptorMessageHaiku::setDescriptors(std::span<const int> descriptors)
{
    m_size = 0;
    m_bytes.fill(0);
    if (descriptors.size() > attachmentMaxAmountHaiku)
        return false;
    while (!descriptors.empty()) {
        auto count = std::min(descriptors.size(), descriptorGroupSizeHaiku);
        auto* header = reinterpret_cast<cmsghdr*>(m_bytes.data() + m_size);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(header), descriptors.data(), count * sizeof(int));
        m_size += CMSG_SPACE(count * sizeof(int));
        descriptors = descriptors.subspan(count);
    }
    return true;
}

void FileDescriptorMessageHaiku::attachTo(msghdr& message)
{
    message.msg_control = m_size ? m_bytes.data() : nullptr;
    message.msg_controllen = m_size;
}

MessagePacketHaiku::Result MessagePacketHaiku::receive(int socket)
{
    m_attachments.clear();
    m_sharedBody = nullptr;
    m_bodyOffset = 0;
    m_bodySize = 0;

    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * attachmentMaxAmountHaiku)> control;
    iovec vector { m_bytes.data(), m_bytes.size() };
    msghdr message { };
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t bytes;
    do {
        bytes = recvmsg(socket, &message, MSG_NOSIGNAL | MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? Result::WouldBlock : Result::Error;

    // Adopt all returned descriptors before validating the envelope, including
    // truncated packets. RAII closes them on every rejection path.
    Vector<UnixFileDescriptor> descriptors;
    bool invalidControl = false;
    for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
        auto offset = reinterpret_cast<uint8_t*>(item) - control.data();
        if (item->cmsg_len < CMSG_LEN(0) || offset + item->cmsg_len > message.msg_controllen) {
            invalidControl = true;
            break;
        }
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS) {
            invalidControl = true;
            continue;
        }
        auto length = item->cmsg_len - CMSG_LEN(0);
        if (length % sizeof(int))
            invalidControl = true;
        for (size_t i = 0; i < length / sizeof(int); ++i) {
            int descriptor;
            memcpy(&descriptor, CMSG_DATA(item) + i * sizeof(int), sizeof(descriptor));
            descriptors.append(UnixFileDescriptor { descriptor, UnixFileDescriptor::Adopt });
        }
    }
    if (invalidControl || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        || static_cast<size_t>(bytes) > m_bytes.size() || descriptors.size() > attachmentMaxAmountHaiku)
        return Result::Error;
    if (!bytes)
        return Result::Closed;
    if (!decode(bytes, WTF::move(descriptors))) {
        m_attachments.clear();
        m_sharedBody = nullptr;
        return Result::Error;
    }
    return Result::Message;
}

bool MessagePacketHaiku::decode(size_t bytes, Vector<UnixFileDescriptor>&& descriptors)
{
    if (bytes < sizeof(MessageInfo))
        return false;
    MessageInfo info;
    memcpySpan(asMutableByteSpan(info), std::span { m_bytes }.first(sizeof(info)));
    if (!info.isValid() || info.attachmentCount() > attachmentMaxAmountHaiku || !info.bodySize())
        return false;
    auto metadataSize = info.attachmentCount() * sizeof(AttachmentInfoHaiku);
    if (metadataSize > bytes - sizeof(info))
        return false;
    auto offset = sizeof(info) + metadataSize;
    if (info.isBodyOutOfLine()) {
        if (!info.attachmentCount() || bytes != offset)
            return false;
    } else if (info.bodySize() != bytes - offset)
        return false;

    size_t descriptorCount = 0;
    for (size_t i = 0; i < info.attachmentCount(); ++i) {
        AttachmentInfoHaiku attachment;
        memcpy(&attachment, m_bytes.data() + sizeof(info) + i, sizeof(attachment));
        if (!attachment.isValid())
            return false;
        descriptorCount += !attachment.isNull;
    }
    if (descriptorCount != descriptors.size())
        return false;

    auto attachmentCount = info.attachmentCount() - (info.isBodyOutOfLine() ? 1 : 0);
    if (info.isBodyOutOfLine() && m_bytes[sizeof(info) + attachmentCount])
        return false;
    m_attachments.resize(attachmentCount);
    size_t descriptorIndex = 0;
    for (size_t i = 0; i < attachmentCount; ++i) {
        if (!m_bytes[sizeof(info) + i])
            m_attachments[attachmentCount - i - 1] = WTF::move(descriptors[descriptorIndex++]);
    }
    if (info.isBodyOutOfLine()) {
        WebCore::SharedMemory::Handle handle { WTF::move(descriptors[descriptorIndex]), info.bodySize() };
        m_sharedBody = WebCore::SharedMemory::map(WTF::move(handle), WebCore::SharedMemory::Protection::ReadOnly);
        if (!m_sharedBody)
            return false;
    }
    m_bodyOffset = offset;
    m_bodySize = info.bodySize();
    return true;
}

std::span<const uint8_t> MessagePacketHaiku::body() const
{
    if (m_sharedBody)
        return m_sharedBody->span().first(m_bodySize);
    return std::span { m_bytes }.subspan(m_bodyOffset, m_bodySize);
}

} // namespace IPC
