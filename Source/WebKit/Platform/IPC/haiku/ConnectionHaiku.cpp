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
#include "Connection.h"

#include "IPCUtilities.h"
#include "MessagePacketHaiku.h"
#include "UnixMessage.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <wtf/UniStdExtras.h>

namespace IPC {

int Connection::socketDescriptor() const
{
    return m_socketDescriptor.value();
}

void Connection::platformInitialize(Identifier&& identifier)
{
    m_socketDescriptor = WTF::move(identifier.handle);
}

void Connection::platformInvalidate()
{
    m_isConnected = false;
    m_socketMonitor = nullptr;
    m_writeSocketMonitor = nullptr;
    m_hasPendingOutputMessage = false;
    m_socketDescriptor = { };
}

bool Connection::platformPrepareForOpen()
{
    return setNonBlock(socketDescriptor());
}

void Connection::platformOpen()
{
    m_connectionQueue->dispatch([protectedThis = Ref { *this }] {
        if (!protectedThis->isValid())
            return;
        protectedThis->m_isConnected = true;
        protectedThis->m_socketMonitor = SocketMonitorHaiku::create(protectedThis->socketDescriptor(),
            protectedThis->m_connectionQueue, [protectedThis] {
                if (protectedThis->m_isConnected)
                    protectedThis->readyReadHandler();
            });
        if (!protectedThis->m_socketMonitor) {
            protectedThis->connectionDidClose();
            return;
        }
        protectedThis->sendOutgoingMessages();
    });
}

void Connection::readyReadHandler()
{
    MessagePacketHaiku packet;
    // Return to the queue periodically so traffic cannot indefinitely postpone
    // cancellation or outgoing messages. Level-triggered polling resumes reads.
    for (size_t i = 0; i < 64 && m_isConnected && isValid(); ++i) {
        switch (packet.receive(socketDescriptor())) {
        case MessagePacketHaiku::Result::WouldBlock:
            return;
        case MessagePacketHaiku::Result::Closed:
        case MessagePacketHaiku::Result::Error:
            connectionDidClose();
            return;
        case MessagePacketHaiku::Result::Message:
            break;
        }
        auto decoder = Decoder::create(packet.body(), packet.takeAttachments());
        if (!decoder) {
            connectionDidClose();
            return;
        }
        processIncomingMessage(makeUniqueRefFromNonNullUniquePtr(WTF::move(decoder)));
    }
}

bool Connection::platformCanSendOutgoingMessages() const
{
    return !m_hasPendingOutputMessage;
}

bool Connection::sendOutgoingMessage(UniqueRef<Encoder>&& encoder)
{
    UnixMessage message(encoder.get());
    if (message.attachments().size() >= attachmentMaxAmountHaiku) {
        connectionDidClose();
        return false;
    }
    auto inlineOverhead = sizeof(MessageInfo) + message.attachments().size() * sizeof(AttachmentInfoHaiku);
    if (message.bodySize() > messageMaxSizeHaiku - inlineOverhead && !message.setBodyOutOfLine()) {
        connectionDidClose();
        return false;
    }
    return sendOutputMessage(WTF::move(message));
}

bool Connection::sendOutputMessage(UnixMessage&& output)
{
    ASSERT(!m_hasPendingOutputMessage);
    auto& info = output.messageInfo();
    const auto& attachments = output.attachments();
    std::array<AttachmentInfoHaiku, attachmentMaxAmountHaiku> metadata;
    std::array<int, attachmentMaxAmountHaiku> descriptors;
    FileDescriptorMessageHaiku control;
    std::array<iovec, 3> vectors;
    vectors[0] = { &info, sizeof(info) };
    size_t vectorCount = 1;
    size_t packetSize = sizeof(info);
    size_t descriptorCount = 0;
    msghdr message { };

    for (size_t i = 0; i < attachments.size(); ++i) {
        metadata[i].isNull = !attachments[i];
        if (attachments[i]) {
            descriptors[descriptorCount++] = attachments[i].value();
        }
    }
    if (!control.setDescriptors(std::span { descriptors }.first(descriptorCount))) {
        connectionDidClose();
        return false;
    }
    control.attachTo(message);
    if (!attachments.isEmpty()) {
        auto size = sizeof(AttachmentInfoHaiku) * attachments.size();
        vectors[vectorCount++] = { metadata.data(), size };
        packetSize += size;
    }
    if (!info.isBodyOutOfLine()) {
        vectors[vectorCount++] = { output.body().data(), output.bodySize() };
        packetSize += output.bodySize();
    }
    message.msg_iov = vectors.data();
    message.msg_iovlen = vectorCount;

    ssize_t sent;
    do {
        sent = sendmsg(socketDescriptor(), &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    if (sent >= 0) {
        if (static_cast<size_t>(sent) == packetSize)
            return true;
        connectionDidClose();
        return false;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        connectionDidClose();
        return false;
    }

    // Retain the unsent packet and its descriptors while waiting asynchronously
    // for capacity. The connection queue remains available for invalidation.
    m_hasPendingOutputMessage = true;
    m_writeSocketMonitor = SocketMonitorHaiku::create(socketDescriptor(), m_connectionQueue,
        [this, protectedThis = Ref { *this }, output = WTF::move(output)]() mutable {
            m_writeSocketMonitor = nullptr;
            // Writable readiness may mean space for fewer bytes than this
            // whole packet needs. Yield between retries instead of spinning.
            m_connectionQueue->dispatchAfter(1_ms, [this, protectedThis, output = WTF::move(output)]() mutable {
                m_hasPendingOutputMessage = false;
                if (m_isConnected && isValid() && sendOutputMessage(WTF::move(output)))
                    sendOutgoingMessages();
            });
        }, SocketMonitorHaiku::Mode::Write);
    if (!m_writeSocketMonitor)
        connectionDidClose();
    return false;
}

std::optional<Connection::ConnectionIdentifierPair> Connection::createConnectionIdentifierPair()
{
    auto pair = createPlatformConnection(SOCK_SEQPACKET);
    return { { Identifier { WTF::move(pair.server) }, ConnectionHandle { WTF::move(pair.client) } } };
}

} // namespace IPC
