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
#include "AudioMediaStreamTrackRendererHaiku.h"

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "HaikuAudioData.h"
#include "HaikuVoiceProcessing.h"
#include <MediaDefs.h>
#include <SoundPlayer.h>
#include <wtf/MainThread.h>

namespace WebCore {

// How much may wait: a stream that runs ahead of the sound card loses the
// oldest samples beyond this.
static constexpr double maximumLatency = 0.25;
// After running dry, output waits for this much before playing again, so a
// late packet does not turn into a crackle of tiny gaps.
static constexpr double restartThreshold = 0.04;

AudioMediaStreamTrackRendererHaiku::AudioMediaStreamTrackRendererHaiku(Init&& init)
    : AudioMediaStreamTrackRenderer(WTF::move(init))
{
}

AudioMediaStreamTrackRendererHaiku::~AudioMediaStreamTrackRendererHaiku()
{
    closeOutput();
}

void AudioMediaStreamTrackRendererHaiku::start(CompletionHandler<void()>&& callback)
{
    m_started = true;
    callback();
}

void AudioMediaStreamTrackRendererHaiku::stop()
{
    m_started = false;
    closeOutput();
    clear();
}

void AudioMediaStreamTrackRendererHaiku::clear()
{
    Locker locker { m_lock };
    m_readIndex = 0;
    m_available = 0;
    m_waitingForData = true;
}

void AudioMediaStreamTrackRendererHaiku::setVolume(float volume)
{
    AudioMediaStreamTrackRenderer::setVolume(volume);
    m_gain = volume;
}

void AudioMediaStreamTrackRendererHaiku::pushSamples(const MediaTime&, const PlatformAudioData& data, const AudioStreamDescription& description, size_t frameCount)
{
    if (!m_started || !is<HaikuAudioData>(data))
        return;
    auto& audio = downcast<HaikuAudioData>(data);
    unsigned channels = audio.channelCount();
    double rate = description.sampleRate();
    if (!channels || rate <= 0)
        return;
    frameCount = std::min(frameCount, audio.frameCount());
    auto samples = audio.samples().first(frameCount * channels);

    bool needsOutput = false;
    {
        Locker locker { m_lock };
        if (rate != m_sampleRate || channels != m_channelCount) {
            m_sampleRate = rate;
            m_channelCount = channels;
            m_fifo.fill(0, static_cast<size_t>(rate * maximumLatency) * channels);
            m_readIndex = 0;
            m_available = 0;
            m_waitingForData = true;
            needsOutput = true;
        }
        size_t capacity = m_fifo.size();
        if (!capacity)
            return;
        if (samples.size() > capacity)
            samples = samples.last(capacity - capacity % channels);
        // Forget the oldest samples that do not fit.
        if (m_available + samples.size() > capacity) {
            size_t drop = m_available + samples.size() - capacity;
            m_readIndex = (m_readIndex + drop) % capacity;
            m_available -= drop;
        }
        size_t writeIndex = (m_readIndex + m_available) % capacity;
        size_t first = std::min(samples.size(), capacity - writeIndex);
        memcpy(m_fifo.mutableSpan().subspan(writeIndex).data(), samples.data(), first * sizeof(float));
        if (first < samples.size())
            memcpy(m_fifo.mutableSpan().data(), samples.subspan(first).data(), (samples.size() - first) * sizeof(float));
        m_available += samples.size();
    }

    // The output is opened on the main thread, once per format.
    if (needsOutput || (!m_outputOpen && !m_outputPending.exchange(true))) {
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }, rate, channels] {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            protectedThis->m_outputPending = false;
            if (protectedThis->m_started)
                protectedThis->openOutput(rate, channels);
        });
    }
}

void AudioMediaStreamTrackRendererHaiku::openOutput(double sampleRate, unsigned channelCount)
{
    if (m_output && m_outputRate == sampleRate && m_outputChannels == channelCount)
        return;
    closeOutput();

    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate = sampleRate;
    format.channel_count = channelCount;
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    // 10 ms buffers, as the streams come.
    format.buffer_size = static_cast<size_t>(sampleRate / 100) * channelCount * sizeof(float);

    auto* output = new BSoundPlayer(&format, "Summit stream audio", playBuffer, nullptr, this);
    if (output->InitCheck() != B_OK) {
        fprintf(stderr, "Summit: stream audio output failed: %s\n", strerror(output->InitCheck()));
        delete output;
        return;
    }
    m_output = output;
    m_outputRate = sampleRate;
    m_outputChannels = channelCount;
    m_output->Start();
    m_output->SetHasData(true);
    m_outputOpen = true;
}

void AudioMediaStreamTrackRendererHaiku::closeOutput()
{
    if (!m_output)
        return;
    m_outputOpen = false;
    m_output->Stop();
    delete m_output;
    m_output = nullptr;
}

void AudioMediaStreamTrackRendererHaiku::playBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format)
{
    auto* self = static_cast<AudioMediaStreamTrackRendererHaiku*>(cookie);
    if (format.format != media_raw_audio_format::B_AUDIO_FLOAT) {
        memset(buffer, 0, size);
        return;
    }
    self->fill(static_cast<float*>(buffer), size / sizeof(float));
}

void AudioMediaStreamTrackRendererHaiku::fill(float* output, size_t count)
{
    size_t copied = 0;
    {
        Locker locker { m_lock };
        size_t capacity = m_fifo.size();
        if (capacity && m_channelCount) {
            if (m_waitingForData && m_available >= static_cast<size_t>(m_sampleRate * restartThreshold) * m_channelCount)
                m_waitingForData = false;
            if (!m_waitingForData) {
                copied = std::min(count, m_available);
                size_t first = std::min(copied, capacity - m_readIndex);
                memcpy(output, m_fifo.span().subspan(m_readIndex).data(), first * sizeof(float));
                if (first < copied)
                    memcpy(output + first, m_fifo.span().data(), (copied - first) * sizeof(float));
                m_readIndex = (m_readIndex + copied) % capacity;
                m_available -= copied;
                if (copied < count)
                    m_waitingForData = true;
            }
        }
    }
    float gain = m_gain;
    if (gain != 1) {
        for (size_t i = 0; i < copied; ++i)
            output[i] *= gain;
    }
    if (copied < count)
        memset(output + copied, 0, (count - copied) * sizeof(float));

#if USE(LIBWEBRTC)
    // What the speakers play is the echo the microphone's processing removes.
    if (copied && HaikuVoiceProcessing::isAvailable()) {
        unsigned channels;
        double rate;
        {
            Locker locker { m_lock };
            channels = m_channelCount;
            rate = m_sampleRate;
        }
        if (channels && rate > 0)
            HaikuVoiceProcessing::singleton().analyzeRender(std::span<const float>(output, count), channels, static_cast<unsigned>(rate));
    }
#endif
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
