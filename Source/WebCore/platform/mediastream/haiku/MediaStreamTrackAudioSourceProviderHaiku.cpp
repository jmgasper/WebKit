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
#include "MediaStreamTrackAudioSourceProviderHaiku.h"

#if ENABLE(WEB_AUDIO) && ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "AudioBus.h"
#include "AudioChannel.h"
#include "AudioSourceProviderClient.h"
#include "HaikuAudioData.h"
#include <wtf/MainThread.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

// A live source: what does not fit is dropped from the old end.
static constexpr double fifoDuration = 0.2;

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaStreamTrackAudioSourceProviderHaiku);

Ref<MediaStreamTrackAudioSourceProviderHaiku> MediaStreamTrackAudioSourceProviderHaiku::create(MediaStreamTrackPrivate& track)
{
    return adoptRef(*new MediaStreamTrackAudioSourceProviderHaiku(track));
}

MediaStreamTrackAudioSourceProviderHaiku::MediaStreamTrackAudioSourceProviderHaiku(MediaStreamTrackPrivate& track)
    : m_track(track)
    , m_source(track.source())
{
}

MediaStreamTrackAudioSourceProviderHaiku::~MediaStreamTrackAudioSourceProviderHaiku()
{
    m_source->removeAudioSampleObserver(*this);
    if (RefPtr track = m_track.get())
        track->removeObserver(*this);
}

void MediaStreamTrackAudioSourceProviderHaiku::setClient(WeakPtr<AudioSourceProviderClient>&& client)
{
    m_client = WTF::move(client);
    bool shouldBeConnected = !!m_client;
    if (m_connected == shouldBeConnected)
        return;
    m_connected = shouldBeConnected;
    RefPtr track = m_track.get();
    if (!shouldBeConnected) {
        if (track)
            track->removeObserver(*this);
        m_source->removeAudioSampleObserver(*this);
        return;
    }
    if (track) {
        m_enabled = track->enabled();
        track->addObserver(*this);
    }
    m_source->addAudioSampleObserver(*this);
}

void MediaStreamTrackAudioSourceProviderHaiku::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& data, const AudioStreamDescription& description, size_t frameCount)
{
    if (!is<HaikuAudioData>(data))
        return;
    auto& audio = downcast<HaikuAudioData>(data);
    unsigned channels = audio.channelCount();
    double rate = description.sampleRate();
    if (!channels || rate <= 0)
        return;
    frameCount = std::min(frameCount, audio.frameCount());
    auto samples = audio.samples().first(frameCount * channels);

    bool formatChanged = false;
    {
        Locker locker { m_lock };
        if (channels != m_channelCount || rate != m_sampleRate) {
            m_channelCount = channels;
            m_sampleRate = rate;
            m_fifo.fill(0, static_cast<size_t>(rate * fifoDuration) * channels);
            m_readIndex = 0;
            m_available = 0;
            formatChanged = true;
        }
        size_t capacity = m_fifo.size();
        if (samples.size() > capacity)
            samples = samples.last(capacity - capacity % channels);
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

    if (formatChanged) {
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }, channels, rate] {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            if (RefPtr client = protectedThis->m_client.get())
                client->setFormat(channels, rate);
        });
    }
}

void MediaStreamTrackAudioSourceProviderHaiku::provideInput(AudioBus& bus, size_t framesToProcess)
{
    if (m_ended || !m_enabled || !m_lock.tryLock()) {
        bus.zero();
        return;
    }
    Locker locker { AdoptLock, m_lock };
    unsigned channels = m_channelCount;
    size_t capacity = m_fifo.size();
    if (!channels || !capacity || bus.numberOfChannels() != channels) {
        bus.zero();
        return;
    }
    size_t frames = std::min(framesToProcess, m_available / channels);
    for (size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels; ++channel)
            bus.channel(channel)->mutableSpan()[frame] = m_fifo[(m_readIndex + channel) % capacity];
        m_readIndex = (m_readIndex + channels) % capacity;
    }
    m_available -= frames * channels;
    for (unsigned channel = 0; channel < channels; ++channel) {
        auto span = bus.channel(channel)->mutableSpan();
        if (frames < framesToProcess && frames < span.size())
            std::fill(span.begin() + frames, span.begin() + std::min(framesToProcess, span.size()), 0.0f);
    }
}

} // namespace WebCore

#endif
