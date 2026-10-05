/*
 * Copyright (C) 2014 Haiku, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef MediaPlayerPrivateHaiku_h
#define MediaPlayerPrivateHaiku_h
#if ENABLE(VIDEO)

#include "MediaPlayerPrivate.h"

#include <wtf/WeakPtr.h>
#include <atomic>
#include <memory>
#include <optional>

#include <Locker.h>
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
#include <skia/core/SkData.h>
#endif
#include <MediaDefs.h>
#include <ObjectList.h>
#include <private/netservices/UrlProtocolAsynchronousListener.h>

class BBitmap;
class BDataIO;
class BFile;
class BMediaFile;
class BMediaTrack;
class BSoundPlayer;

namespace WebCore {

// Reads the Media Kit's file formats on a thread of its own (process start).
void prepareMediaEngineTypesHaiku();

class MediaPlayerFactoryHaiku;

class SubstituteVideoDecoderHaiku;
class PlatformMediaResource;

class MediaPlayerPrivate
    : public MediaPlayerPrivateInterface
    , public CanMakeWeakPtr<MediaPlayerPrivate>
    , public RefCounted<MediaPlayerPrivate>
{
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MediaPlayerPrivate);
public:
        void ref() const final { RefCounted::ref(); }
        void deref() const final { RefCounted::deref(); }


        friend class MediaPlayerFactoryHaiku;

        static void registerMediaEngine(MediaEngineRegistrar);

        MediaPlayerPrivate(MediaPlayer&);
        MediaPlayerPrivate() = delete;

        ~MediaPlayerPrivate();

        void load(const String& url) override;
#if ENABLE(MEDIA_SOURCE)
        void load(const URL& url, const LoadOptions&, MediaSourcePrivateClient&) override;
#endif
#if ENABLE(MEDIA_STREAM)
        // Streams play in MediaPlayerPrivateMediaStreamHaiku.
        void load(MediaStreamPrivate&) override { }
#endif
        void cancelLoad() override;

        void prepareToPlay() override;

        void play() override;
        void pause() override;

        FloatSize naturalSize() const override;
        bool hasAudio() const override;
        bool hasVideo() const override;

        void setPageIsVisible(bool) override;

        WTF::MediaTime duration() const override;
        WTF::MediaTime currentTime() const override;

        Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
        bool paused() const override;
        bool ended() const override { return m_ended; }

        void setVolume(float) override;

        MediaPlayer::NetworkState networkState() const override;
        MediaPlayer::ReadyState readyState() const override;

        WTF::MediaTime maxTimeSeekable() const override { return duration(); }

        const PlatformTimeRanges& buffered() const override;
        bool didLoadingProgress() const override;

        void paint(GraphicsContext&, const FloatRect&) override;
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
        // Decoded frames go straight to the compositor, so a busy page thread
        // does not hold them back.
        bool supportsAcceleratedRendering() const override { return true; }
        void setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&&) override;
        RefPtr<CoordinatedPlatformLayerBufferProxy> platformLayerBufferProxy() const override;
#endif
        ColorSpace colorSpace() override;

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::Haiku; }
        void setMuted(bool) override;
private:
    private:
        void IdentifyTracks(const String& url);
        void startVideoPlaybackIfNeeded(bool forSeek = false);
        void startAudioPlaybackIfNeeded();
        void releaseAudioOutput();
        static status_t loadingThread(void* cookie);
        static status_t videoThread(void* cookie);
        void queueVideoFrameRepaint();
        // Called with m_mediaLock held. False when the frame has to be painted.
        bool pushFrameToCompositor();
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
        mutable BLocker m_layerBufferProxyLock;
        RefPtr<CoordinatedPlatformLayerBufferProxy> m_layerBufferProxy;
        // Pixels of the frames handed to the compositor, reused once no image
        // refers to them (m_mediaLock).
        Vector<sk_sp<SkData>> m_compositorBuffers;
#endif


        static void playCallback(void*, void*, size_t,
            const media_raw_audio_format&);

        // engine support
        static void getSupportedTypes(WTF::HashSet<WTF::String>&);
        static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

        mutable bool m_didReceiveData;
        BMediaFile* m_mediaFile;
        BMediaFile* m_secondaryMediaFile { nullptr };
        // A downloaded file, or a StreamingMediaIO that fetches as it is read.
        BPositionIO* m_downloadFile { nullptr };
        BFile* m_secondaryDownloadFile { nullptr };
        BMediaTrack* m_audioTrack;
        BMediaTrack* m_videoTrack;
        // Decodes m_videoTrack in place of its own decoder: the Raspberry Pi's
        // add-on, or libavcodec where the track's own cannot (NVDEC and 16
        // reference frames, or a decoder that failed to set up).
        std::unique_ptr<SubstituteVideoDecoderHaiku> m_substituteVideo;
        BMediaFile* m_videoTrackFile { nullptr };
        int m_videoTrackIndex { -1 };
        media_raw_audio_format m_audioFormat { };
        bool m_mediaTracksReady { false };
        bool m_audioAtEnd { false };
        bool m_videoAtEnd { false };
        BSoundPlayer* m_soundPlayer;
        BBitmap* m_frameBuffer;
        BLocker m_mediaLock;
        PlatformTimeRanges m_buffered;
        // A seek's keyframe preroll runs on the decoder thread, one frame per
        // lock acquisition, so a later seek can replace it without blocking
        // the WebProcess main thread.
        std::atomic<bigtime_t> m_prerollTarget { -1 };
        std::atomic<bool> m_videoSeekStarting { false };
        bigtime_t m_prerollKeyframeTime { 0 };
        unsigned m_prerollFrames { 0 };
        std::atomic<unsigned> m_seekGeneration { 0 };
        std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
        std::atomic<unsigned> m_frameTraceSequence { 0 };
        std::atomic<unsigned> m_lastFrameTracePaintedSequence { 0 };
        std::atomic<bigtime_t> m_lastFrameTraceDecodedAt { 0 };
        std::atomic<bigtime_t> m_lastFrameTraceDecodeDuration { 0 };
        std::atomic<bigtime_t> m_frameTraceRepaintQueuedAt { 0 };
        std::atomic<unsigned> m_frameTraceRepaintQueuedSequence { 0 };
        std::atomic<unsigned> m_videoFrameGeneration { 0 };
        std::atomic<bool> m_videoRepaintQueued { false };
#if USE(CURL)
        BLocker m_downloadLock;
        void* m_activeDownload { nullptr };
#endif

        MediaPlayer& m_player;
        MediaPlayer::NetworkState m_networkState;
        MediaPlayer::ReadyState m_readyState;

        float m_volume;
        bool m_muted { false };
        std::atomic<float> m_currentTime;
        std::atomic<float> m_duration { 0 };
        std::atomic<bool> m_hasVideo { false };
        std::atomic<bool> m_ended { false };
        std::atomic<bool> m_paused;
        std::atomic<bool> m_abort;

        thread_id m_loadingThread;
        thread_id m_videoThread;
        String m_url;
        // The page's, for the requests that fetch the media; read on the page
        // thread when loading starts.
        std::string m_userAgent;
        std::string m_referrer;
        String m_downloadPath;
        String m_secondaryDownloadPath;

        // A blob: or data: URL, fetched whole through WebCore into
        // m_downloadPath before the loading thread starts (load()).
        bool startPrefetchHaiku(const URL&);
        void didFinishPrefetchHaiku(bool succeeded);
        RefPtr<PlatformMediaResource> m_prefetchResource;
        unsigned m_prefetchGeneration { 0 };
        bool m_prefetchedHaiku { false };
};


}

#endif
#endif
