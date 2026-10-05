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

#include "AudioMediaStreamTrackRenderer.h"
#include <wtf/Lock.h>
#include <wtf/Vector.h>

class BSoundPlayer;
struct media_raw_audio_format;

namespace WebCore {

// Plays a stream's audio track (a WebRTC peer, a microphone a page listens
// to) through a BSoundPlayer of its own, which the system mixer mixes with
// everything else. Samples wait in a short FIFO: a stream is live, so what
// arrives late is dropped rather than played late.
class AudioMediaStreamTrackRendererHaiku final : public AudioMediaStreamTrackRenderer {
public:
    static Ref<AudioMediaStreamTrackRendererHaiku> create(Init&& init) { return adoptRef(*new AudioMediaStreamTrackRendererHaiku(WTF::move(init))); }
    ~AudioMediaStreamTrackRendererHaiku();

private:
    explicit AudioMediaStreamTrackRendererHaiku(Init&&);

    void start(CompletionHandler<void()>&&) final;
    void stop() final;
    void clear() final;
    void pushSamples(const MediaTime&, const PlatformAudioData&, const AudioStreamDescription&, size_t) final;
    void setVolume(float) final;

    static void playBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format&);
    void fill(float* output, size_t samples);
    void openOutput(double sampleRate, unsigned channelCount);
    void closeOutput();

    Lock m_lock;
    Vector<float> m_fifo WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_readIndex WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    size_t m_available WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_waitingForData WTF_GUARDED_BY_LOCK(m_lock) { true };
    double m_sampleRate WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_channelCount WTF_GUARDED_BY_LOCK(m_lock) { 0 };

    std::atomic<bool> m_started { false };
    std::atomic<bool> m_outputPending { false };
    std::atomic<bool> m_outputOpen { false };
    std::atomic<float> m_gain { 1 };
    // Main thread.
    BSoundPlayer* m_output { nullptr };
    double m_outputRate { 0 };
    unsigned m_outputChannels { 0 };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
