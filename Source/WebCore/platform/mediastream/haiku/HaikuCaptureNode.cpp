/*
 * Copyright (C) 2026 Summit contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#include "config.h"
#include "HaikuCaptureNode.h"

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include <Buffer.h>
#include <BufferConsumer.h>
#include <MediaEventLooper.h>
#include <MediaRoster.h>
#include <OS.h>
#include <TimeSource.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

namespace WebCore {

static bool captureTraceEnabled()
{
    static bool enabled = [] {
        const char* value = getenv("SUMMIT_CAPTURE_TRACE");
        return value && *value && strcmp(value, "0");
    }();
    return enabled;
}

#define CAPTURE_TRACE(...) do { if (captureTraceEnabled()) fprintf(stderr, "Summit capture: " __VA_ARGS__); } while (0)

// Our end of a capture connection: takes buffers as they come and hands them
// to the connection's handler at once (capture wants them now, not at a
// performance time), then gives them back to the producer.
class HaikuCaptureConsumer final : public BMediaEventLooper, public BBufferConsumer {
public:
    HaikuCaptureConsumer(const char* name, media_type type)
        : BMediaNode(name)
        , BMediaEventLooper()
        , BBufferConsumer(type)
        , m_type(type)
    {
        m_input.destination.port = -1;
    }

    void setHandler(HaikuCaptureConnection::BufferHandler&& handler)
    {
        Locker locker { m_handlerLock };
        m_handler = WTF::move(handler);
    }

    media_input input() const { return m_input; }

    BMediaAddOn* AddOn(int32*) const final { return nullptr; }

    void NodeRegistered() final
    {
        m_input.destination.port = ControlPort();
        m_input.destination.id = 0;
        m_input.source = media_source::null;
        m_input.node = Node();
        m_input.format.type = m_type;
        if (m_type == B_MEDIA_RAW_VIDEO)
            m_input.format.u.raw_video = media_raw_video_format::wildcard;
        else
            m_input.format.u.raw_audio = media_raw_audio_format::wildcard;
        strlcpy(m_input.name, Name(), sizeof(m_input.name));
        Run();
    }

    status_t AcceptFormat(const media_destination&, media_format* format) final
    {
        if (format->type != m_type)
            return B_MEDIA_BAD_FORMAT;
        return B_OK;
    }

    status_t GetNextInput(int32* cookie, media_input* input) final
    {
        if (*cookie)
            return B_BAD_INDEX;
        *input = m_input;
        ++*cookie;
        return B_OK;
    }

    void DisposeInputCookie(int32) final { }

    void BufferReceived(BBuffer* buffer) final
    {
        {
            Locker locker { m_handlerLock };
            if (m_handler)
                m_handler(buffer->Data(), buffer->SizeUsed(), m_input.format, buffer->Header()->start_time);
        }
        buffer->Recycle();
    }

    void ProducerDataStatus(const media_destination&, int32, bigtime_t) final { }

    status_t GetLatencyFor(const media_destination&, bigtime_t* latency, media_node_id* timeSource) final
    {
        *latency = 5000;
        *timeSource = TimeSource()->ID();
        return B_OK;
    }

    status_t Connected(const media_source& source, const media_destination&, const media_format& format, media_input* input) final
    {
        m_input.source = source;
        m_input.format = format;
        *input = m_input;
        return B_OK;
    }

    void Disconnected(const media_source&, const media_destination&) final
    {
        m_input.source = media_source::null;
    }

    status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format& format) final
    {
        if (format.type != m_type)
            return B_MEDIA_BAD_FORMAT;
        m_input.format = format;
        return B_OK;
    }

    void HandleEvent(const media_timed_event*, bigtime_t, bool) final { }

    status_t HandleMessage(int32, const void*, size_t) final { return B_ERROR; }

private:
    media_type m_type;
    media_input m_input;
    Lock m_handlerLock;
    HaikuCaptureConnection::BufferHandler m_handler WTF_GUARDED_BY_LOCK(m_handlerLock);
};

static BMediaRoster* mediaRoster()
{
    status_t status = B_OK;
    BMediaRoster* roster = BMediaRoster::Roster(&status);
    if (!roster)
        CAPTURE_TRACE("no media roster: %s\n", strerror(status));
    return roster;
}

Vector<HaikuMediaInput> haikuMediaInputs(media_type type)
{
    Vector<HaikuMediaInput> inputs;
    BMediaRoster* roster = mediaRoster();
    if (!roster)
        return inputs;

    media_format format;
    format.type = type;
    if (type == B_MEDIA_RAW_VIDEO)
        format.u.raw_video = media_raw_video_format::wildcard;
    else
        format.u.raw_audio = media_raw_audio_format::wildcard;

    live_node_info nodes[64];
    int32 count = std::size(nodes);
    if (roster->GetLiveNodes(nodes, &count, nullptr, &format, nullptr, B_BUFFER_PRODUCER | B_PHYSICAL_INPUT) != B_OK)
        return inputs;

    media_node defaultNode;
    media_node_id defaultId = -1;
    if ((type == B_MEDIA_RAW_VIDEO ? roster->GetVideoInput(&defaultNode) : roster->GetAudioInput(&defaultNode)) == B_OK) {
        defaultId = defaultNode.node;
        roster->ReleaseNode(defaultNode);
    }

    HashMap<String, unsigned> nameCounts;
    for (int32 index = 0; index < count; ++index) {
        auto& info = nodes[index];
        if (info.node.kind & B_SYSTEM_MIXER)
            continue;
        String name = String::fromUTF8(info.name);
        if (name.isEmpty())
            name = type == B_MEDIA_RAW_VIDEO ? "Camera"_s : "Microphone"_s;
        unsigned occurrence = ++nameCounts.add(name, 0).iterator->value;
        HaikuMediaInput input;
        input.persistentId = occurrence == 1 ? makeString("haiku:"_s, name) : makeString("haiku:"_s, name, '#', occurrence);
        input.label = occurrence == 1 ? name : makeString(name, " ("_s, occurrence, ')');
        input.node = info.node;
        input.isDefault = info.node.node == defaultId;
        if (input.isDefault)
            inputs.insert(0, WTF::move(input));
        else
            inputs.append(WTF::move(input));
    }
    if (!inputs.isEmpty() && defaultId < 0)
        inputs[0].isDefault = true;
    CAPTURE_TRACE("%u %s input(s)\n", unsigned(inputs.size()), type == B_MEDIA_RAW_VIDEO ? "video" : "audio");
    return inputs;
}

std::optional<HaikuMediaInput> haikuMediaInputWithId(media_type type, const String& persistentId)
{
    for (auto& input : haikuMediaInputs(type)) {
        if (input.persistentId == persistentId)
            return input;
    }
    return std::nullopt;
}

// Outputs still connected to a consumer whose program has ended (it crashed,
// or was killed while capturing): the media services do not break those
// connections, and the device would stay in use until they restart. A port
// goes with its team, so a destination port that is gone marks one.
static void reclaimAbandonedOutputs(BMediaRoster& roster, const media_node& producer)
{
    media_output outputs[16];
    int32 count = 0;
    if (roster.GetConnectedOutputsFor(producer, outputs, std::size(outputs), &count) != B_OK)
        return;
    for (int32 index = 0; index < count; ++index) {
        port_info info;
        if (get_port_info(outputs[index].destination.port, &info) == B_OK)
            continue;
        CAPTURE_TRACE("output \"%s\" of node %d was left connected by a program that ended; disconnecting it\n",
            outputs[index].name, int(producer.node));
        // The consumer's node went with it; Disconnect() wants a valid id
        // only to republish that node's inputs.
        roster.Disconnect(producer.node, outputs[index].source, producer.node, outputs[index].destination);
    }
}

// The connections that are up, for disconnectAllBeforeExit(). Disconnecting
// happens under the lock, so a connection is never disconnected twice.
static Lock liveConnectionsLock;
static HashSet<HaikuCaptureConnection*>& liveConnections() WTF_REQUIRES_LOCK(liveConnectionsLock)
{
    static NeverDestroyed<HashSet<HaikuCaptureConnection*>> connections;
    return connections;
}

std::unique_ptr<HaikuCaptureConnection> HaikuCaptureConnection::connect(const media_node& producer, const media_format& requested, BufferHandler&& handler, String& error)
{
    BMediaRoster* roster = mediaRoster();
    if (!roster) {
        error = "The media services are not running."_s;
        return nullptr;
    }

    media_output output;
    int32 count = 0;
    status_t status = roster->GetFreeOutputsFor(producer, &output, 1, &count, requested.type);
    if (status == B_OK && count < 1) {
        reclaimAbandonedOutputs(*roster, producer);
        status = roster->GetFreeOutputsFor(producer, &output, 1, &count, requested.type);
    }
    if (status != B_OK || count < 1) {
        // Cameras serve one consumer at a time.
        error = "The device is in use by another program."_s;
        CAPTURE_TRACE("no free output on node %d: %s\n", int(producer.node), strerror(status));
        return nullptr;
    }

    std::unique_ptr<HaikuCaptureConnection> connection(new HaikuCaptureConnection);
    connection->m_producer = producer;
    connection->m_output = output;

    auto* consumer = new HaikuCaptureConsumer(requested.type == B_MEDIA_RAW_VIDEO ? "Summit camera" : "Summit microphone", requested.type);
    status = roster->RegisterNode(consumer);
    if (status != B_OK) {
        error = "The media services refused a capture node."_s;
        consumer->Release();
        return nullptr;
    }
    connection->m_consumer = consumer;
    consumer->setHandler(WTF::move(handler));

    media_input input;
    count = 0;
    roster->GetFreeInputsFor(consumer->Node(), &input, 1, &count, requested.type);
    if (count < 1) {
        error = "The capture node has no input."_s;
        return nullptr;
    }
    connection->m_input = input;

    media_format format = requested;
    status = roster->Connect(output.source, input.destination, &format, &connection->m_output, &connection->m_input);
    if (status != B_OK) {
        error = makeString("Connecting to the device failed: "_s, String::fromLatin1(strerror(status)));
        CAPTURE_TRACE("connect failed: %s\n", strerror(status));
        return nullptr;
    }
    connection->m_format = format;
    {
        Locker locker { liveConnectionsLock };
        connection->m_connected = true;
        liveConnections().add(connection.get());
    }

    // Follow the producer's clock, as its buffers are stamped with it.
    BTimeSource* timeSource = roster->MakeTimeSourceFor(producer);
    bigtime_t start = 0;
    if (timeSource) {
        roster->SetTimeSourceFor(consumer->ID(), timeSource->ID());
        start = timeSource->Now() + 20000;
        timeSource->Release();
    }
    roster->StartNode(consumer->Node(), start);
    // A sound card's input is usually running already (it plays too); a
    // camera is started for us.
    roster->StartNode(producer, start);

    if (format.type == B_MEDIA_RAW_VIDEO) {
        CAPTURE_TRACE("video connected: %ux%u, %.1f per second, color space %d, \"%s\"\n",
            unsigned(format.u.raw_video.display.line_width), unsigned(format.u.raw_video.display.line_count),
            format.u.raw_video.field_rate, int(format.u.raw_video.display.format), connection->m_output.name);
    } else {
        CAPTURE_TRACE("audio connected: %.0f Hz, %u channel(s), format 0x%x, buffers of %u bytes, \"%s\"\n",
            format.u.raw_audio.frame_rate, unsigned(format.u.raw_audio.channel_count), unsigned(format.u.raw_audio.format),
            unsigned(format.u.raw_audio.buffer_size), connection->m_output.name);
    }
    return connection;
}

void HaikuCaptureConnection::disconnectLocked()
{
    liveConnections().remove(this);
    BMediaRoster* roster = BMediaRoster::CurrentRoster();
    if (!roster || !m_connected)
        return;
    m_connected = false;
    // A sound card's node also plays: only a camera is stopped. Without its
    // consumer a camera stops by itself anyway.
    if (m_format.type == B_MEDIA_RAW_VIDEO)
        roster->StopNode(m_producer, 0, true);
    if (m_consumer)
        roster->StopNode(m_consumer->Node(), 0, true);
    roster->Disconnect(m_producer.node, m_output.source, m_consumer->ID(), m_input.destination);
    CAPTURE_TRACE("%s disconnected\n", m_format.type == B_MEDIA_RAW_VIDEO ? "video" : "audio");
}

void HaikuCaptureConnection::disconnectAllBeforeExit()
{
    {
        Locker locker { liveConnectionsLock };
        if (liveConnections().isEmpty())
            return;
    }
    // On a thread of its own: the media services may not answer, and the
    // process ends anyway.
    sem_id done = create_sem(0, "Summit capture disconnect");
    if (done < B_OK)
        return;
    thread_id thread = spawn_thread([](void* data) -> status_t {
        {
            Locker locker { liveConnectionsLock };
            for (auto* connection : copyToVector(liveConnections()))
                connection->disconnectLocked();
        }
        release_sem(static_cast<sem_id>(reinterpret_cast<intptr_t>(data)));
        return B_OK;
    }, "Summit capture disconnect", B_NORMAL_PRIORITY, reinterpret_cast<void*>(static_cast<intptr_t>(done)));
    if (thread >= B_OK && resume_thread(thread) == B_OK)
        acquire_sem_etc(done, 1, B_RELATIVE_TIMEOUT, 500000);
    delete_sem(done);
}

HaikuCaptureConnection::~HaikuCaptureConnection()
{
    if (m_consumer)
        m_consumer->setHandler(nullptr);
    {
        Locker locker { liveConnectionsLock };
        disconnectLocked();
    }
    // Registered nodes go through the roster, as CodyCam does it.
    if (m_consumer) {
        if (BMediaRoster* roster = BMediaRoster::CurrentRoster())
            roster->ReleaseNode(m_consumer->Node());
        else
            m_consumer->Release();
    }
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
