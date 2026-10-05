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

#if USE(LIBWEBRTC) && PLATFORM(HAIKU)

#include "LibWebRTCProviderHaiku.h"
#include "RealtimeIncomingAudioSource.h"
#include "RealtimeIncomingVideoSource.h"
#include "RealtimeOutgoingAudioSource.h"
#include "RealtimeOutgoingVideoSource.h"
#include "VideoFrameHaiku.h"
#include <wtf/CheckedRef.h>
#include <wtf/Lock.h>
#include <wtf/MediaTime.h>
#include <wtf/TZoneMalloc.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/api/video/video_frame_buffer.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// libwebrtc's pictures as Haiku video frames and back, without copying an
// I420 picture either way.
Ref<VideoFrameHaiku> videoFrameFromWebRTCBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>&&, MediaTime, VideoFrame::Rotation, PlatformVideoColorSpace&&);
webrtc::scoped_refptr<webrtc::VideoFrameBuffer> webRTCBufferFromVideoFrame(VideoFrameHaiku&);

class RealtimeIncomingAudioSourceHaiku final : public RealtimeIncomingAudioSource {
public:
    static Ref<RealtimeIncomingAudioSourceHaiku> create(Ref<webrtc::AudioTrackInterface>&& track, String&& trackId)
    {
        return adoptRef(*new RealtimeIncomingAudioSourceHaiku(WTF::move(track), WTF::move(trackId)));
    }
    ~RealtimeIncomingAudioSourceHaiku();

private:
    RealtimeIncomingAudioSourceHaiku(Ref<webrtc::AudioTrackInterface>&&, String&&);

    // webrtc::AudioTrackSinkInterface
    void OnData(const void* audioData, int bitsPerSample, int sampleRate, size_t numberOfChannels, size_t numberOfFrames) final;

    Vector<float> m_samples;
    uint64_t m_numberOfFrames { 0 };
    MediaTime m_baseTime { MediaTime::invalidTime() };
};

class RealtimeIncomingVideoSourceHaiku final : public RealtimeIncomingVideoSource {
public:
    static Ref<RealtimeIncomingVideoSourceHaiku> create(Ref<webrtc::VideoTrackInterface>&& track, String&& trackId)
    {
        return adoptRef(*new RealtimeIncomingVideoSourceHaiku(WTF::move(track), WTF::move(trackId)));
    }
    ~RealtimeIncomingVideoSourceHaiku();

private:
    RealtimeIncomingVideoSourceHaiku(Ref<webrtc::VideoTrackInterface>&&, String&&);

    // webrtc::VideoSinkInterface
    void OnFrame(const webrtc::VideoFrame&) final;
};

class RealtimeOutgoingAudioSourceHaiku final : public RealtimeOutgoingAudioSource, public CanMakeCheckedPtr<RealtimeOutgoingAudioSourceHaiku> {
    WTF_MAKE_TZONE_ALLOCATED(RealtimeOutgoingAudioSourceHaiku);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RealtimeOutgoingAudioSourceHaiku);
public:
    static Ref<RealtimeOutgoingAudioSourceHaiku> create(Ref<MediaStreamTrackPrivate>&& track)
    {
        return adoptRef(*new RealtimeOutgoingAudioSourceHaiku(WTF::move(track)));
    }

private:
    explicit RealtimeOutgoingAudioSourceHaiku(Ref<MediaStreamTrackPrivate>&&);

    // CheckedPtr interface
    uint32_t checkedPtrCount() const final { return CanMakeCheckedPtr::checkedPtrCount(); }
    uint32_t checkedPtrCountWithoutThreadCheck() const final { return CanMakeCheckedPtr::checkedPtrCountWithoutThreadCheck(); }
    void incrementCheckedPtrCount() const final { CanMakeCheckedPtr::incrementCheckedPtrCount(); }
    void decrementCheckedPtrCount() const final { CanMakeCheckedPtr::decrementCheckedPtrCount(); }
    void setDidBeginCheckedPtrDeletion() final { CanMakeCheckedPtr::setDidBeginCheckedPtrDeletion(); }

    // RealtimeMediaSource::AudioSampleObserver
    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData&, const AudioStreamDescription&, size_t) final;

    // Samples wait here, as 16-bit, until a 10 ms chunk is complete.
    Vector<int16_t> m_pending;
    Vector<int16_t> m_chunk;
    double m_sampleRate { 0 };
    unsigned m_channelCount { 0 };
};

class RealtimeOutgoingVideoSourceHaiku final : public RealtimeOutgoingVideoSource {
public:
    static Ref<RealtimeOutgoingVideoSourceHaiku> create(Ref<MediaStreamTrackPrivate>&& track)
    {
        return adoptRef(*new RealtimeOutgoingVideoSourceHaiku(WTF::move(track)));
    }

private:
    explicit RealtimeOutgoingVideoSourceHaiku(Ref<MediaStreamTrackPrivate>&&);

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> createBlackFrame(size_t width, size_t height) final;

    // RealtimeMediaSource::VideoFrameObserver
    void videoFrameAvailable(VideoFrame&, VideoFrameTimeMetadata) final;
};

} // namespace WebCore

#endif // USE(LIBWEBRTC) && PLATFORM(HAIKU)
