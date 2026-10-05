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

#if ENABLE(WEB_AUDIO) && ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "MediaStreamTrackPrivate.h"
#include "RealtimeMediaSource.h"
#include "WebAudioSourceProvider.h"
#include <wtf/CheckedRef.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class AudioSourceProviderClient;

// A stream's audio track as a Web Audio source (MediaStreamAudioSourceNode):
// samples (interleaved float) wait in a short FIFO the audio thread pulls
// from. The node resamples to its context's rate.
class MediaStreamTrackAudioSourceProviderHaiku final
    : public WebAudioSourceProvider
    , MediaStreamTrackPrivateObserver
    , RealtimeMediaSource::AudioSampleObserver
    , public CanMakeCheckedPtr<MediaStreamTrackAudioSourceProviderHaiku> {
    WTF_MAKE_TZONE_ALLOCATED(MediaStreamTrackAudioSourceProviderHaiku);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaStreamTrackAudioSourceProviderHaiku);
public:
    static Ref<MediaStreamTrackAudioSourceProviderHaiku> create(MediaStreamTrackPrivate&);
    ~MediaStreamTrackAudioSourceProviderHaiku();

    void ref() const final { WebAudioSourceProvider::ref(); }
    void deref() const final { WebAudioSourceProvider::deref(); }

private:
    explicit MediaStreamTrackAudioSourceProviderHaiku(MediaStreamTrackPrivate&);

    // CheckedPtr interface
    uint32_t checkedPtrCount() const final { return CanMakeCheckedPtr::checkedPtrCount(); }
    uint32_t checkedPtrCountWithoutThreadCheck() const final { return CanMakeCheckedPtr::checkedPtrCountWithoutThreadCheck(); }
    void incrementCheckedPtrCount() const final { CanMakeCheckedPtr::incrementCheckedPtrCount(); }
    void decrementCheckedPtrCount() const final { CanMakeCheckedPtr::decrementCheckedPtrCount(); }
    void setDidBeginCheckedPtrDeletion() final { CanMakeCheckedPtr::setDidBeginCheckedPtrDeletion(); }

    // AudioSourceProvider
    void provideInput(AudioBus&, size_t) final;
    void setClient(WeakPtr<AudioSourceProviderClient>&&) final;

    // MediaStreamTrackPrivateObserver
    void trackEnded(MediaStreamTrackPrivate&) final { m_ended = true; }
    void trackMutedChanged(MediaStreamTrackPrivate&) final { }
    void trackSettingsChanged(MediaStreamTrackPrivate&) final { }
    void trackEnabledChanged(MediaStreamTrackPrivate& track) final { m_enabled = track.enabled(); }

    // RealtimeMediaSource::AudioSampleObserver
    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData&, const AudioStreamDescription&, size_t) final;

    WeakPtr<MediaStreamTrackPrivate> m_track;
    const Ref<RealtimeMediaSource> m_source;
    WeakPtr<AudioSourceProviderClient> m_client;
    bool m_connected { false };
    std::atomic<bool> m_enabled { true };
    std::atomic<bool> m_ended { false };

    Lock m_lock;
    Vector<float> m_fifo WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_readIndex WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    size_t m_available WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_channelCount WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    double m_sampleRate WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace WebCore

#endif
