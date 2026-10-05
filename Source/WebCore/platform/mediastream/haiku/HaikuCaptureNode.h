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

#pragma once

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include <MediaDefs.h>
#include <MediaNode.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class HaikuCaptureConsumer;

// A camera or microphone as the Media Kit knows it: a live node that is a
// physical input producing raw video or raw audio.
struct HaikuMediaInput {
    // Stable across restarts (node IDs are not): the node's name, numbered
    // when several inputs share it.
    String persistentId;
    String label;
    media_node node;
    bool isDefault { false };
};

// The camera or microphone inputs, the default one (BMediaRoster's video or
// audio input) first. Talks to the media_server; call it on the main thread.
Vector<HaikuMediaInput> haikuMediaInputs(media_type);
std::optional<HaikuMediaInput> haikuMediaInputWithId(media_type, const String& persistentId);

// Takes buffers from one output of a capture node: a consumer node of ours,
// connected to the producer, both started. The handler runs on the
// consumer's thread with the buffer's data, its size, the connection's format
// and the buffer's start time (performance time of the producer's time
// source). Destroying the connection stops and disconnects both nodes and
// frees the input for others.
class HaikuCaptureConnection {
    WTF_MAKE_NONCOPYABLE(HaikuCaptureConnection);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HaikuCaptureConnection);
public:
    using BufferHandler = Function<void(const void* data, size_t size, const media_format&, bigtime_t startTime)>;

    // `requested` may hold wildcards; for video, a line_width/line_count asks
    // the camera for that size. On failure `error` says why (the input
    // being in use is one reason: it serves one consumer at a time).
    static std::unique_ptr<HaikuCaptureConnection> connect(const media_node& producer, const media_format& requested, BufferHandler&&, String& error);
    ~HaikuCaptureConnection();

    const media_format& format() const { return m_format; }

    // For a process about to end at once (_exit): disconnects every
    // connection still up, waiting half a second at most. The media services
    // do not break a dead program's connections, and a sound card's input
    // would stay in use for every program until they restart.
    WEBCORE_EXPORT static void disconnectAllBeforeExit();

private:
    HaikuCaptureConnection() = default;
    void disconnectLocked();

    media_node m_producer;
    HaikuCaptureConsumer* m_consumer { nullptr };
    media_output m_output;
    media_input m_input;
    media_format m_format;
    bool m_connected { false };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
