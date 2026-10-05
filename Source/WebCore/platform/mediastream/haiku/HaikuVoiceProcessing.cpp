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
#include "HaikuVoiceProcessing.h"

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU) && USE(LIBWEBRTC)

#include "LibWebRTCMacros.h"
#include <cstdlib>
#include <cstring>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <webrtc/api/audio/audio_processing.h>
#include <webrtc/api/audio/builtin_audio_processing_builder.h>
#include <webrtc/api/environment/environment_factory.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// From a sample reaching the sound player to it leaving the speakers, plus
// the microphone's buffer: a starting point AEC3 refines.
static constexpr int initialStreamDelayMs = 60;

bool HaikuVoiceProcessing::isAvailable()
{
    static bool available = [] {
        const char* value = getenv("SUMMIT_VOICE_PROCESSING");
        return !value || strcmp(value, "0");
    }();
    return available;
}

HaikuVoiceProcessing& HaikuVoiceProcessing::singleton()
{
    static NeverDestroyed<HaikuVoiceProcessing> processing;
    return processing;
}

HaikuVoiceProcessing::HaikuVoiceProcessing()
{
    auto processing = webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
    // Kept for the life of the process.
    m_processing = processing.release();
}

void HaikuVoiceProcessing::configure(bool echoCancellation, bool noiseSuppression, bool autoGainControl)
{
    Locker locker { m_configurationLock };
    if (m_configured && echoCancellation == m_echoCancellation && noiseSuppression == m_noiseSuppression && autoGainControl == m_autoGainControl)
        return;
    m_configured = true;
    m_echoCancellation = echoCancellation;
    m_noiseSuppression = noiseSuppression;
    m_autoGainControl = autoGainControl;

    webrtc::AudioProcessing::Config config;
    config.high_pass_filter.enabled = echoCancellation || noiseSuppression;
    config.echo_canceller.enabled = echoCancellation;
    config.noise_suppression.enabled = noiseSuppression;
    config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    config.gain_controller2.enabled = autoGainControl;
    config.gain_controller2.adaptive_digital.enabled = autoGainControl;
    m_processing->ApplyConfig(config);
}

void HaikuVoiceProcessing::processCapture(std::span<float> samples, unsigned channels, unsigned sampleRate, bool echoCancellation, bool noiseSuppression, bool autoGainControl)
{
    if (!m_processing || !channels || !sampleRate || samples.size() != size_t(sampleRate / 100) * channels)
        return;
    if (!echoCancellation && !noiseSuppression && !autoGainControl)
        return;
    configure(echoCancellation, noiseSuppression, autoGainControl);

    size_t frames = sampleRate / 100;
    m_capturePlanes.resize(frames * channels);
    Vector<float*, 8> planes;
    for (unsigned channel = 0; channel < channels; ++channel) {
        float* plane = m_capturePlanes.mutableSpan().subspan(channel * frames).data();
        for (size_t frame = 0; frame < frames; ++frame)
            plane[frame] = samples[frame * channels + channel];
        planes.append(plane);
    }

    m_processing->set_stream_delay_ms(initialStreamDelayMs);
    webrtc::StreamConfig config(sampleRate, channels);
    if (m_processing->ProcessStream(planes.span().data(), config, config, planes.span().data()) != webrtc::AudioProcessing::kNoError)
        return;

    for (unsigned channel = 0; channel < channels; ++channel) {
        const float* plane = planes[channel];
        for (size_t frame = 0; frame < frames; ++frame)
            samples[frame * channels + channel] = plane[frame];
    }
}

void HaikuVoiceProcessing::analyzeRender(std::span<const float> samples, unsigned channels, unsigned sampleRate)
{
    if (!m_processing || !channels || !sampleRate || samples.empty())
        return;
    m_lastRender = MonotonicTime::now().secondsSinceEpoch().value();

    Locker locker { m_renderLock };
    if (channels != m_renderChannels || sampleRate != m_renderRate) {
        m_renderChannels = channels;
        m_renderRate = sampleRate;
        m_renderPending.clear();
    }
    m_renderPending.append(samples);

    size_t frames = sampleRate / 100;
    size_t chunk = frames * channels;
    size_t offset = 0;
    m_renderPlanes.resize(chunk);
    Vector<float*, 8> planes;
    for (unsigned channel = 0; channel < channels; ++channel)
        planes.append(m_renderPlanes.mutableSpan().subspan(channel * frames).data());
    webrtc::StreamConfig config(sampleRate, channels);
    while (m_renderPending.size() - offset >= chunk) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            for (size_t frame = 0; frame < frames; ++frame)
                planes[channel][frame] = m_renderPending[offset + frame * channels + channel];
        }
        m_processing->ProcessReverseStream(planes.span().data(), config, config, planes.span().data());
        offset += chunk;
    }
    if (offset)
        m_renderPending.removeAt(0, offset);
}

} // namespace WebCore

#endif
