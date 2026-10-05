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

#if ENABLE(MEDIA_SOURCE)

// Media Source Extensions on Haiku.
//
// SourceBufferPrivateHaiku parses appended fragmented MP4 into samples and
// leaves buffering, eviction and seeking to WebCore's SourceBufferPrivate.
// Samples it is asked to enqueue go to a renderer per track: video is decoded
// through Media Kit (so H.264 reaches the NVDEC add-on where it is installed)
// and presented against the player's clock; audio is decoded on its own thread
// and played by a BSoundPlayer that follows the same clock.

#include "FragmentedMP4ParserHaiku.h"
#include "MediaPlayerPrivate.h"
#include "MediaSample.h"
#include "MediaSourcePrivate.h"
#include "SourceBufferPrivate.h"
#include <atomic>
#include <memory>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MediaTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WeakPtr.h>

class BBitmap;

namespace WebCore {

class MediaSourcePrivateHaiku;
class HaikuMSEVideoRenderer;
class HaikuMSEAudioRenderer;

// The codec and container details of one track, shared by every sample that
// came after its initialization segment. A new initialization segment makes a
// new configuration, which is how a renderer notices a quality switch.
class HaikuTrackConfiguration final : public ThreadSafeRefCounted<HaikuTrackConfiguration> {
public:
    static Ref<HaikuTrackConfiguration> create(const SummitMP4::TrackInfo& info) { return adoptRef(*new HaikuTrackConfiguration(info)); }
    const SummitMP4::TrackInfo info;

private:
    explicit HaikuTrackConfiguration(const SummitMP4::TrackInfo& info)
        : info(info)
    {
    }
};

class HaikuSampleData final : public ThreadSafeRefCounted<HaikuSampleData> {
public:
    static Ref<HaikuSampleData> create(std::vector<uint8_t>&& bytes) { return adoptRef(*new HaikuSampleData(WTF::move(bytes))); }
    const std::vector<uint8_t> bytes;

private:
    explicit HaikuSampleData(std::vector<uint8_t>&& bytes)
        : bytes(WTF::move(bytes))
    {
    }
};

class MediaSampleHaiku final : public MediaSample {
public:
    static Ref<MediaSampleHaiku> create(Ref<HaikuSampleData>&& data, Ref<HaikuTrackConfiguration>&& configuration, TrackID trackID,
        const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, SampleFlags flags)
    {
        return adoptRef(*new MediaSampleHaiku(WTF::move(data), WTF::move(configuration), trackID, presentationTime, decodeTime, duration, flags));
    }

    MediaTime presentationTime() const final { return m_presentationTime; }
    MediaTime decodeTime() const final { return m_decodeTime; }
    MediaTime duration() const final { return m_duration; }
    TrackID trackID() const final { return m_trackID; }
    size_t sizeInBytes() const final { return m_data->bytes.size() + sizeof(*this); }
    FloatSize presentationSize() const final;
    void offsetTimestampsBy(const MediaTime&) final;
    void setTimestamps(const MediaTime&, const MediaTime&) final;
    Ref<MediaSample> createNonDisplayingCopy() const final;
    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime&) const final;
    SampleFlags flags() const final { return m_flags; }
    PlatformSample platformSample() const final { return PlatformSample { static_cast<const MockSampleBox*>(nullptr) }; }
    Type type() const final { return Type::None; }

    const uint8_t* bytes() const { return m_data->bytes.data(); }
    size_t size() const { return m_data->bytes.size(); }
    const HaikuTrackConfiguration& configuration() const { return m_configuration.get(); }

private:
    MediaSampleHaiku(Ref<HaikuSampleData>&&, Ref<HaikuTrackConfiguration>&&, TrackID, const MediaTime&, const MediaTime&, const MediaTime&, SampleFlags);

    const Ref<HaikuSampleData> m_data;
    const Ref<HaikuTrackConfiguration> m_configuration;
    TrackID m_trackID;
    MediaTime m_presentationTime;
    MediaTime m_decodeTime;
    MediaTime m_duration;
    SampleFlags m_flags;
};

// Media time that advances with the system clock while playback is running.
// Read from the decoder and sound threads, changed on the main thread.
class HaikuMSEClock {
public:
    // Microseconds of media time.
    int64_t now() const;
    bool isRunning() const { return m_running; }
    void start();
    void stop();
    void set(int64_t mediaTime);

private:
    mutable Lock m_lock;
    int64_t m_base WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    int64_t m_startedAt WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    std::atomic<bool> m_running { false };
};

class MediaPlayerPrivateHaikuMSE final
    : public MediaPlayerPrivateInterface
    , public RefCounted<MediaPlayerPrivateHaikuMSE>
    , public CanMakeWeakPtr<MediaPlayerPrivateHaikuMSE> {
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    explicit MediaPlayerPrivateHaikuMSE(MediaPlayer&);
    ~MediaPlayerPrivateHaikuMSE();

    static void registerMediaEngine(MediaEngineRegistrar);
    static void getSupportedTypes(HashSet<String>&);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::HaikuMSE; }

    // Used by the source buffers.
    HaikuMSEVideoRenderer& videoRenderer();
    HaikuMSEAudioRenderer& audioRenderer();
    void updateDuration(const MediaTime&);
    void notifyActiveSourceBuffersChanged();
    const HaikuMSEClock& clock() const { return m_clock; }

    // Called on the main thread by the renderers.
    void videoFrameAvailable();

#if !RELEASE_LOG_DISABLED
    uint64_t mediaPlayerLogIdentifier();
    const Logger& mediaPlayerLogger();
#endif

private:
    void load(const String&) final;
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final { }
#endif
    void cancelLoad() final;
    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }
    FloatSize naturalSize() const final;
    bool hasVideo() const final;
    bool hasAudio() const final;
    void setPageIsVisible(bool) final { }
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    MediaTime currentTime() const final;
    bool timeIsProgressing() const final;
    MediaTime duration() const final;
    MediaTime maxTimeSeekable() const final { return duration(); }
    void setVolume(float) final;
    void setMuted(bool) final;
    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final;
    void readyStateFromMediaSourceChanged() final;
    void characteristicsFromMediaSourceChanged() final;
    void seekableRangesFromMediaSourceChanged() final;
    void mediaSourceHasRetrievedAllData() final;
    const PlatformTimeRanges& buffered() const final;
    bool didLoadingProgress() const final { return false; }
    void paint(GraphicsContext&, const FloatRect&) final;
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    // Frames go straight from the decoder thread to the compositor.
    bool supportsAcceleratedRendering() const final { return true; }
    void setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&&) final;
    RefPtr<CoordinatedPlatformLayerBufferProxy> platformLayerBufferProxy() const final { return m_layerBufferProxy; }
    RefPtr<CoordinatedPlatformLayerBufferProxy> m_layerBufferProxy;
#endif
    ColorSpace colorSpace() final;
    std::optional<VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;

    void setNetworkState(MediaPlayer::NetworkState);
    void updatePlaybackState();
    void playbackTimerFired();

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RefPtr<MediaSourcePrivateHaiku> m_mediaSourcePrivate;
    std::unique_ptr<HaikuMSEVideoRenderer> m_videoRenderer;
    std::unique_ptr<HaikuMSEAudioRenderer> m_audioRenderer;
    HaikuMSEClock m_clock;
    RunLoop::Timer m_playbackTimer;

    MediaTime m_duration { MediaTime::invalidTime() };
    std::optional<SeekTarget> m_pendingSeek;
    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    bool m_paused { true };
    bool m_stalled { false };
    bool m_reachedEnd { false };
    bool m_announcedFirstFrame { false };
    unsigned m_statsTicks { 0 };
    MonotonicTime m_prerollStartedAt;
    float m_volume { 1 };
    bool m_muted { false };
    FloatSize m_naturalSize;
};

class MediaSourcePrivateHaiku final : public MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateHaiku> create(MediaPlayerPrivateHaikuMSE&, MediaSourcePrivateClient&);
    virtual ~MediaSourcePrivateHaiku();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Haiku; }
    RefPtr<MediaPlayerPrivateInterface> player() const final;
    void setPlayer(MediaPlayerPrivateInterface*) final;
    RefPtr<MediaPlayerPrivateHaikuMSE> haikuPlayer() const { return m_player.get(); }

private:
    MediaSourcePrivateHaiku(MediaPlayerPrivateHaikuMSE&, MediaSourcePrivateClient&);

    AddStatus addSourceBuffer(const ContentType&, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>&) final;
    void durationChanged(const MediaTime&) final;
    void notifyActiveSourceBuffersChanged() final;

    WeakPtr<MediaPlayerPrivateHaikuMSE> m_player;
};

class SourceBufferPrivateHaiku final : public SourceBufferPrivate {
public:
    static Ref<SourceBufferPrivateHaiku> create(MediaSourcePrivateHaiku&);
    virtual ~SourceBufferPrivateHaiku();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Haiku; }

    // Called by a renderer, on any thread, when it can take samples again.
    void rendererReadyForMoreSamples(TrackID);

private:
    explicit SourceBufferPrivateHaiku(MediaSourcePrivateHaiku&);

    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    bool canSwitchToType(const ContentType&) final;
    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    void allSamplesInTrackEnqueued(TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;
    void notifyClientWhenReadyForMoreSamples(TrackID) final;
    void updateTrackIds(Vector<std::pair<TrackID, TrackID>>&&) final;
    void removedFromMediaSource() final;

    void didParseInitializationSegment(SummitMP4::InitSegment&&);
    void didParseSample(SummitMP4::Sample&&);
    RefPtr<MediaPlayerPrivateHaikuMSE> player() const;
    enum class Renderer { None, Video, Audio };
    Renderer rendererForTrack(TrackID) const;

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger.get(); }
    ASCIILiteral logClassName() const final { return "SourceBufferPrivateHaiku"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;
    const Logger& sourceBufferLogger() const final { return m_logger.get(); }
    uint64_t sourceBufferLogIdentifier() final { return logIdentifier(); }
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif

    SummitMP4::Parser m_parser;
    bool m_parseFailed { false };
    // Parser track number -> configuration from the latest init segment.
    HashMap<uint64_t, Ref<HaikuTrackConfiguration>> m_configurations;
    // First track of each kind in this buffer; only those are played.
    std::optional<TrackID> m_videoTrackID;
    std::optional<TrackID> m_audioTrackID;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaPlayerPrivateHaikuMSE)
static bool isType(const WebCore::MediaPlayerPrivateInterface& player) { return player.mediaPlayerType() == WebCore::MediaPlayerType::HaikuMSE; }
SPECIALIZE_TYPE_TRAITS_END()

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaSourcePrivateHaiku)
static bool isType(const WebCore::MediaSourcePrivate& mediaSource) { return mediaSource.platformType() == WebCore::MediaPlatformType::Haiku; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(MEDIA_SOURCE)
