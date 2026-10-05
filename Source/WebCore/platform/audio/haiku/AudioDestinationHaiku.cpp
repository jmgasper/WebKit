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
#include "AudioDestinationHaiku.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"
#include "AudioUtilities.h"
#include "SharedAudioDestination.h"
#if ENABLE(MEDIA_STREAM) && USE(LIBWEBRTC)
#include "HaikuVoiceProcessing.h"
#endif
#include <MediaRoster.h>
#include <SoundPlayer.h>
#include <wtf/MainThread.h>

namespace WebCore {

// Frames per BSoundPlayer buffer: four render quanta, ~10.7 ms at 48 kHz.
static constexpr size_t outputBufferFrames = 4 * AudioUtilities::renderQuantumSize;

Ref<AudioDestination> AudioDestination::create(const CreationOptions& options)
{
    // One output per page shared by its contexts, as on Cocoa.
    return SharedAudioDestination::create(options, [](const CreationOptions& options) {
        return adoptRef(*new AudioDestinationHaiku(options));
    });
}

float AudioDestination::hardwareSampleRate()
{
    // The system mixer's rate; it resamples whatever it gets, so this only
    // spares a conversion.
    static float rate = [] {
        float rate = 48000;
        BMediaRoster* roster = BMediaRoster::Roster();
        media_node mixer;
        if (roster && roster->GetAudioMixer(&mixer) == B_OK) {
            media_output output;
            int32 count = 0;
            if (roster->GetAllOutputsFor(mixer, &output, 1, &count) == B_OK && count > 0
                && output.format.type == B_MEDIA_RAW_AUDIO && output.format.u.raw_audio.frame_rate > 0)
                rate = output.format.u.raw_audio.frame_rate;
            roster->ReleaseNode(mixer);
        }
        return rate;
    }();
    return rate;
}

unsigned long AudioDestination::maxChannelCount()
{
    // Stereo only, as the system mixer mixes.
    return 0;
}

AudioDestinationHaiku::AudioDestinationHaiku(const CreationOptions& options)
    : AudioDestinationResampler(options, hardwareSampleRate())
{
}

AudioDestinationHaiku::~AudioDestinationHaiku()
{
    if (m_output) {
        m_output->Stop();
        delete m_output;
    }
}

void AudioDestinationHaiku::startRendering(CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(isMainThread());
    bool success = true;
    if (!m_output) {
        unsigned channels = numberOfOutputChannels();
        media_raw_audio_format format = media_raw_audio_format::wildcard;
        format.frame_rate = hardwareSampleRate();
        format.channel_count = channels;
        format.format = media_raw_audio_format::B_AUDIO_FLOAT;
        format.byte_order = B_MEDIA_HOST_ENDIAN;
        format.buffer_size = outputBufferFrames * channels * sizeof(float);
        m_planes.resize(outputBufferFrames * channels);

        auto* output = new BSoundPlayer(&format, "Summit Web Audio", playBuffer, nullptr, this);
        if (output->InitCheck() == B_OK)
            m_output = output;
        else {
            fprintf(stderr, "Summit: Web Audio output failed: %s\n", strerror(output->InitCheck()));
            delete output;
            success = false;
        }
    }
    if (m_output) {
        m_output->Start();
        m_output->SetHasData(true);
        setIsPlaying(true);
    }
    callOnMainThread([completionHandler = WTF::move(completionHandler), success]() mutable {
        completionHandler(success);
    });
}

void AudioDestinationHaiku::stopRendering(CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(isMainThread());
    if (m_output)
        m_output->Stop();
    setIsPlaying(false);
    callOnMainThread([completionHandler = WTF::move(completionHandler)]() mutable {
        completionHandler(true);
    });
}

MediaTime AudioDestinationHaiku::outputLatency() const
{
    if (!m_output)
        return MediaTime::zeroTime();
    return MediaTime(m_output->Latency(), 1000000);
}

void AudioDestinationHaiku::playBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format)
{
    auto* self = static_cast<AudioDestinationHaiku*>(cookie);
    unsigned channels = format.channel_count;
    if (format.format != media_raw_audio_format::B_AUDIO_FLOAT || !channels) {
        memset(buffer, 0, size);
        return;
    }
    self->fill(static_cast<float*>(buffer), size / (sizeof(float) * channels), channels);
}

void AudioDestinationHaiku::fill(float* output, size_t frames, unsigned channels)
{
    unsigned busChannels = m_outputBus->numberOfChannels();
    frames = std::min(frames, m_planes.size() / std::max(busChannels, 1u));
    // The bus reads into our planes, one per channel, then they are
    // interleaved into the sound player's buffer.
    for (unsigned channel = 0; channel < busChannels; ++channel)
        m_outputBus->setChannelMemory(channel, m_planes.mutableSpan().subspan(channel * frames, frames));
    auto framesToRender = pullRendered(frames);
    render(m_framesPlayed / sampleRate(), MonotonicTime::now(), framesToRender);
    m_framesPlayed += frames;

    for (size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            unsigned source = std::min(channel, busChannels - 1);
            output[frame * channels + channel] = busChannels ? m_planes[source * frames + frame] : 0;
        }
    }

#if ENABLE(MEDIA_STREAM) && USE(LIBWEBRTC)
    // A call page may play its sound through Web Audio: the microphone's echo
    // cancellation hears it too.
    if (framesToRender && HaikuVoiceProcessing::isAvailable())
        HaikuVoiceProcessing::singleton().analyzeRender(std::span<const float>(output, frames * channels), channels, static_cast<unsigned>(sampleRate()));
#endif
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
