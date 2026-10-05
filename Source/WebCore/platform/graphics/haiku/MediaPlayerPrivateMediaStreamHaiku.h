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

#include "MediaPlayerPrivate.h"
#include "MediaStreamPrivate.h"
#include "MediaStreamTrackPrivate.h"
#include "RealtimeMediaSource.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/RunLoop.h>
#include <wtf/text/StringHash.h>

namespace WebCore {

class AudioTrackPrivateMediaStream;
class CoordinatedPlatformLayerBufferProxy;
class VideoFrameHaiku;
class VideoTrackPrivateMediaStream;

// Plays a MediaStream (a camera, a screen, a WebRTC peer) in a media element:
// the newest picture of the active video track goes to the compositor as it
// arrives, audio tracks play through AudioMediaStreamTrackRendererHaiku.
class MediaPlayerPrivateMediaStreamHaiku final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateMediaStreamHaiku, WTF::DestructionThread::Main>
    , private MediaStreamPrivateObserver
    , public MediaStreamTrackPrivateObserver
    , public RealtimeMediaSource::VideoFrameObserver {
public:
    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    explicit MediaPlayerPrivateMediaStreamHaiku(MediaPlayer&);
    ~MediaPlayerPrivateMediaStreamHaiku();

    static void registerMediaEngine(MediaEngineRegistrar);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::HaikuMediaStream; }

    USING_CAN_MAKE_WEAKPTR(MediaStreamTrackPrivateObserver);

private:
    // MediaPlayerPrivateInterface
    void load(const String&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#endif
    void load(MediaStreamPrivate&) final;
    void cancelLoad() final;
    void play() final;
    void pause() final;
    bool paused() const final { return !m_playing; }
    FloatSize naturalSize() const final;
    bool hasVideo() const final;
    bool hasAudio() const final;
    void setPageIsVisible(bool) final { }
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    MediaTime currentTime() const final;
    MediaTime duration() const final { return MediaTime::positiveInfiniteTime(); }
    MediaTime maxTimeSeekable() const final { return MediaTime::zeroTime(); }
    bool timeIsProgressing() const final { return m_playing; }
    void setVolume(float) final;
    void setMuted(bool) final;
    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    const PlatformTimeRanges& buffered() const final;
    bool didLoadingProgress() const final { return true; }
    void paint(GraphicsContext&, const FloatRect&) final;
    RefPtr<VideoFrame> videoFrameForCurrentTime() final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    ColorSpace colorSpace() final;
    bool supportsFullscreen() const final { return true; }
    bool didPassCORSAccessCheck() const final { return true; }
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    bool supportsAcceleratedRendering() const final { return true; }
    void setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&&) final;
    RefPtr<CoordinatedPlatformLayerBufferProxy> platformLayerBufferProxy() const final;
#endif

    // MediaStreamPrivateObserver
    void characteristicsChanged() final;
    void activeStatusChanged() final;
    void didAddTrack(MediaStreamTrackPrivate&) final;
    void didRemoveTrack(MediaStreamTrackPrivate&) final;

    // MediaStreamTrackPrivateObserver
    void trackEnded(MediaStreamTrackPrivate&) final;
    void trackMutedChanged(MediaStreamTrackPrivate&) final;
    void trackSettingsChanged(MediaStreamTrackPrivate&) final;
    void trackEnabledChanged(MediaStreamTrackPrivate&) final;
    void readyStateChanged(MediaStreamTrackPrivate&) final;

    // RealtimeMediaSource::VideoFrameObserver, on the source's thread.
    void videoFrameAvailable(VideoFrame&, VideoFrameTimeMetadata) final;

    void updateTracks();
    void updateActiveVideoTrack();
    void updateReadyState();
    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    void frameArrived();
    RefPtr<VideoFrameHaiku> currentFrame() const;

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RefPtr<MediaStreamPrivate> m_mediaStreamPrivate;
    HashMap<String, Ref<AudioTrackPrivateMediaStream>> m_audioTracks;
    HashMap<String, Ref<VideoTrackPrivateMediaStream>> m_videoTracks;
    RefPtr<MediaStreamTrackPrivate> m_activeVideoTrack;

    mutable Lock m_frameLock;
    RefPtr<VideoFrameHaiku> m_frame WTF_GUARDED_BY_LOCK(m_frameLock);
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    RefPtr<CoordinatedPlatformLayerBufferProxy> m_layerBufferProxy WTF_GUARDED_BY_LOCK(m_frameLock);
#endif
    std::atomic<bool> m_frameNotificationPending { false };
    std::atomic<bool> m_playingForFrames { false };
    bool m_hasFrame { false };
    FloatSize m_naturalSize;

    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    bool m_playing { false };
    bool m_muted { false };
    float m_volume { 1 };
    MonotonicTime m_startTime;
    MediaTime m_pausedTime { MediaTime::zeroTime() };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
