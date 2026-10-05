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
#include "MediaPlayerPrivateMediaStreamHaiku.h"

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "AudioTrackPrivateMediaStream.h"
#include "ColorSpace.h"
#include "GraphicsContext.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include "PlatformTimeRanges.h"
#include "VideoFrameHaiku.h"
#include "VideoTrackPrivateMediaStream.h"
#include <wtf/MainThread.h>

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#include "CoordinatedPlatformLayerBufferProxy.h"
#include "CoordinatedPlatformLayerBufferVideoFrameHaiku.h"
#endif

namespace WebCore {

class MediaPlayerFactoryHaikuMediaStream final : public MediaPlayerFactory {
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::HaikuMediaStream; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final { return adoptRef(*new MediaPlayerPrivateMediaStreamHaiku(player)); }
    void getSupportedTypes(HashSet<String>&) const final { }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final { return MediaPlayerPrivateMediaStreamHaiku::supportsType(parameters); }
};

void MediaPlayerPrivateMediaStreamHaiku::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryHaikuMediaStream>());
}

MediaPlayer::SupportsType MediaPlayerPrivateMediaStreamHaiku::supportsType(const MediaEngineSupportParameters& parameters)
{
    return parameters.platformType == PlatformMediaDecodingType::MediaStream ? MediaPlayer::SupportsType::IsSupported : MediaPlayer::SupportsType::IsNotSupported;
}

MediaPlayerPrivateMediaStreamHaiku::MediaPlayerPrivateMediaStreamHaiku(MediaPlayer& player)
    : m_player(player)
{
}

MediaPlayerPrivateMediaStreamHaiku::~MediaPlayerPrivateMediaStreamHaiku()
{
    if (m_activeVideoTrack)
        m_activeVideoTrack->source().removeVideoFrameObserver(*this);
    for (auto& track : m_audioTracks.values()) {
        track->streamTrack().removeObserver(*this);
        track->clear();
    }
    for (auto& track : m_videoTracks.values())
        track->streamTrack().removeObserver(*this);
    if (m_mediaStreamPrivate)
        m_mediaStreamPrivate->removeObserver(*this);
}

void MediaPlayerPrivateMediaStreamHaiku::load(const String&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateMediaStreamHaiku::load(const URL&, const LoadOptions&, MediaSourcePrivateClient&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateMediaStreamHaiku::load(MediaStreamPrivate& stream)
{
    m_mediaStreamPrivate = &stream;
    stream.addObserver(*this);
    m_hasFrame = false;
    setNetworkState(MediaPlayer::NetworkState::Idle);
    updateTracks();
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::cancelLoad()
{
    if (m_playing)
        pause();
}

void MediaPlayerPrivateMediaStreamHaiku::play()
{
    if (m_playing)
        return;
    m_playing = true;
    m_playingForFrames = true;
    m_startTime = MonotonicTime::now();
    for (auto& track : m_audioTracks.values())
        track->play();
    if (RefPtr player = m_player.get())
        player->rateChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::pause()
{
    if (!m_playing)
        return;
    m_pausedTime = currentTime();
    m_playing = false;
    m_playingForFrames = false;
    for (auto& track : m_audioTracks.values())
        track->pause();
    if (RefPtr player = m_player.get())
        player->rateChanged();
}

MediaTime MediaPlayerPrivateMediaStreamHaiku::currentTime() const
{
    if (!m_playing)
        return m_pausedTime;
    return m_pausedTime + MediaTime::createWithDouble((MonotonicTime::now() - m_startTime).seconds());
}

Ref<MediaTimePromise> MediaPlayerPrivateMediaStreamHaiku::seekToTarget(const SeekTarget&)
{
    return MediaTimePromise::createAndReject(PlatformMediaError::Cancelled);
}

const PlatformTimeRanges& MediaPlayerPrivateMediaStreamHaiku::buffered() const
{
    return PlatformTimeRanges::emptyRanges();
}

FloatSize MediaPlayerPrivateMediaStreamHaiku::naturalSize() const
{
    if (!m_naturalSize.isEmpty())
        return m_naturalSize;
    if (m_activeVideoTrack) {
        auto& settings = m_activeVideoTrack->settings();
        return FloatSize(settings.width(), settings.height());
    }
    return { };
}

bool MediaPlayerPrivateMediaStreamHaiku::hasVideo() const
{
    return m_mediaStreamPrivate && m_mediaStreamPrivate->hasVideo();
}

bool MediaPlayerPrivateMediaStreamHaiku::hasAudio() const
{
    return m_mediaStreamPrivate && m_mediaStreamPrivate->hasAudio();
}

void MediaPlayerPrivateMediaStreamHaiku::setVolume(float volume)
{
    m_volume = volume;
    for (auto& track : m_audioTracks.values())
        track->setVolume(volume);
}

void MediaPlayerPrivateMediaStreamHaiku::setMuted(bool muted)
{
    m_muted = muted;
    for (auto& track : m_audioTracks.values())
        track->setMuted(muted);
}

void MediaPlayerPrivateMediaStreamHaiku::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::updateReadyState()
{
    if (!m_mediaStreamPrivate || !m_mediaStreamPrivate->active()) {
        setReadyState(m_hasFrame ? MediaPlayer::ReadyState::HaveCurrentData : MediaPlayer::ReadyState::HaveNothing);
        return;
    }
    // A video waits for its first picture; audio plays as it comes.
    if (m_activeVideoTrack && !m_hasFrame) {
        setReadyState(m_mediaStreamPrivate->hasAudio() ? MediaPlayer::ReadyState::HaveMetadata : MediaPlayer::ReadyState::HaveNothing);
        return;
    }
    setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
}

void MediaPlayerPrivateMediaStreamHaiku::updateTracks()
{
    RefPtr stream = m_mediaStreamPrivate;
    if (!stream)
        return;
    RefPtr player = m_player.get();
    if (!player)
        return;

    auto currentTracks = stream->tracks();

    // Audio tracks.
    HashSet<String> audioIds;
    for (auto& track : currentTracks) {
        if (!track->isAudio())
            continue;
        audioIds.add(track->id());
        if (m_audioTracks.contains(track->id()))
            continue;
        auto audioTrack = AudioTrackPrivateMediaStream::create(track.get());
        track->addObserver(*this);
        m_audioTracks.add(track->id(), audioTrack.copyRef());
        player->addAudioTrack(audioTrack.get());
    }
    m_audioTracks.removeIf([&](auto& entry) {
        if (audioIds.contains(entry.key))
            return false;
        entry.value->streamTrack().removeObserver(*this);
        entry.value->clear();
        player->removeAudioTrack(entry.value.get());
        return true;
    });
    int index = 0;
    for (auto& track : m_audioTracks.values()) {
        track->setTrackIndex(index++);
        track->setVolume(m_volume);
        track->setMuted(m_muted);
        track->setEnabled(track->streamTrack().enabled() && !track->streamTrack().muted());
        if (m_playing)
            track->play();
    }

    if (!player->isVideoPlayer()) {
        updateActiveVideoTrack();
        return;
    }

    // Video tracks.
    HashSet<String> videoIds;
    for (auto& track : currentTracks) {
        if (!track->isVideo())
            continue;
        videoIds.add(track->id());
        if (m_videoTracks.contains(track->id()))
            continue;
        auto videoTrack = VideoTrackPrivateMediaStream::create(track.get());
        track->addObserver(*this);
        m_videoTracks.add(track->id(), videoTrack.copyRef());
        player->addVideoTrack(videoTrack.get());
    }
    m_videoTracks.removeIf([&](auto& entry) {
        if (videoIds.contains(entry.key))
            return false;
        entry.value->streamTrack().removeObserver(*this);
        player->removeVideoTrack(entry.value.get());
        return true;
    });
    index = 0;
    for (auto& track : m_videoTracks.values()) {
        track->setTrackIndex(index++);
        track->setSelected(&track->streamTrack() == stream->activeVideoTrack());
    }
    updateActiveVideoTrack();
}

void MediaPlayerPrivateMediaStreamHaiku::updateActiveVideoTrack()
{
    RefPtr<MediaStreamTrackPrivate> active;
    RefPtr player = m_player.get();
    if (m_mediaStreamPrivate && player && player->isVideoPlayer())
        active = m_mediaStreamPrivate->activeVideoTrack();
    if (active == m_activeVideoTrack)
        return;

    if (m_activeVideoTrack)
        m_activeVideoTrack->source().removeVideoFrameObserver(*this);
    m_activeVideoTrack = active;
    {
        Locker locker { m_frameLock };
        m_frame = nullptr;
    }
    m_hasFrame = false;
    m_naturalSize = { };
    if (m_activeVideoTrack)
        m_activeVideoTrack->source().addVideoFrameObserver(*this);
    if (player)
        player->sizeChanged();
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::characteristicsChanged()
{
    updateTracks();
    updateReadyState();
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::activeStatusChanged()
{
    updateReadyState();
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::didAddTrack(MediaStreamTrackPrivate&)
{
    updateTracks();
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::didRemoveTrack(MediaStreamTrackPrivate&)
{
    updateTracks();
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::trackEnded(MediaStreamTrackPrivate&)
{
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::trackMutedChanged(MediaStreamTrackPrivate&)
{
    updateTracks();
}

void MediaPlayerPrivateMediaStreamHaiku::trackSettingsChanged(MediaStreamTrackPrivate& track)
{
    if (&track != m_activeVideoTrack.get())
        return;
    if (RefPtr player = m_player.get())
        player->sizeChanged();
}

void MediaPlayerPrivateMediaStreamHaiku::trackEnabledChanged(MediaStreamTrackPrivate&)
{
    updateTracks();
}

void MediaPlayerPrivateMediaStreamHaiku::readyStateChanged(MediaStreamTrackPrivate&)
{
    updateReadyState();
}

void MediaPlayerPrivateMediaStreamHaiku::videoFrameAvailable(VideoFrame& videoFrame, VideoFrameTimeMetadata)
{
    // A paused element keeps its picture, as in other browsers.
    if (!m_playingForFrames && m_hasFrame)
        return;

    RefPtr<VideoFrameHaiku> frame = &downcast<VideoFrameHaiku>(videoFrame);
    if (frame->rotation() != VideoFrame::Rotation::None) {
        // Rare (a phone's camera through WebRTC): turned upright on the CPU.
        frame = frame->uprightCopy();
        if (!frame)
            return;
    }
    if (frame->format() == VideoFrameHaiku::Format::NV12) {
        frame = frame->convertedTo(VideoFrameHaiku::Format::I420);
        if (!frame)
            return;
    }

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    RefPtr<CoordinatedPlatformLayerBufferProxy> proxy;
#endif
    {
        Locker locker { m_frameLock };
        m_frame = frame;
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
        proxy = m_layerBufferProxy;
#endif
    }
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    // Pictures go to the compositor from here, so a busy page does not hold
    // them back.
    if (proxy && proxy->isValid()) {
        if (frame->format() == VideoFrameHaiku::Format::I420)
            proxy->setDisplayBuffer(makeUnique<CoordinatedPlatformLayerBufferVideoFrameHaiku>(frame.releaseNonNull()));
        else if (RefPtr image = frame->nativeImage())
            proxy->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(), nullptr));
    }
#endif

    if (m_frameNotificationPending.exchange(true))
        return;
    callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis.get()) {
            protectedThis->m_frameNotificationPending = false;
            protectedThis->frameArrived();
        }
    });
}

void MediaPlayerPrivateMediaStreamHaiku::frameArrived()
{
    RefPtr player = m_player.get();
    if (!player)
        return;
    auto frame = currentFrame();
    if (!frame)
        return;

    FloatSize size = frame->presentationSize();
    if (size != m_naturalSize) {
        m_naturalSize = size;
        player->sizeChanged();
    }
    if (!m_hasFrame) {
        m_hasFrame = true;
        updateReadyState();
        player->firstVideoFrameAvailable();
    }
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    {
        Locker locker { m_frameLock };
        if (m_layerBufferProxy && m_layerBufferProxy->isValid())
            return;
    }
#endif
    player->repaint();
}

RefPtr<VideoFrameHaiku> MediaPlayerPrivateMediaStreamHaiku::currentFrame() const
{
    Locker locker { m_frameLock };
    return m_frame;
}

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
void MediaPlayerPrivateMediaStreamHaiku::setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&& proxy)
{
    RefPtr<VideoFrameHaiku> frame;
    {
        Locker locker { m_frameLock };
        m_layerBufferProxy = proxy.copyRef();
        frame = m_frame;
    }
    if (!frame)
        return;
    if (frame->format() == VideoFrameHaiku::Format::I420)
        proxy->setInitialDisplayBuffer(makeUnique<CoordinatedPlatformLayerBufferVideoFrameHaiku>(frame.releaseNonNull()));
    else if (RefPtr image = frame->nativeImage())
        proxy->setInitialDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(), nullptr));
}

RefPtr<CoordinatedPlatformLayerBufferProxy> MediaPlayerPrivateMediaStreamHaiku::platformLayerBufferProxy() const
{
    Locker locker { m_frameLock };
    return m_layerBufferProxy;
}
#endif

void MediaPlayerPrivateMediaStreamHaiku::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;
    auto frame = currentFrame();
    if (!frame)
        return;
    context.drawVideoFrame(*frame, rect, ImageOrientation::Orientation::None, false);
}

RefPtr<VideoFrame> MediaPlayerPrivateMediaStreamHaiku::videoFrameForCurrentTime()
{
    return currentFrame();
}

RefPtr<NativeImage> MediaPlayerPrivateMediaStreamHaiku::nativeImageForCurrentTime()
{
    if (auto frame = currentFrame())
        return frame->nativeImage();
    return nullptr;
}

ColorSpace MediaPlayerPrivateMediaStreamHaiku::colorSpace()
{
    return ColorSpace::SRGB();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
