/*
 * Copyright (C) 2014-2016 Haiku, Inc.
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

#include "config.h"
#if USE(SKIA)
#include "NativeImage.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif
#include "ColorSpace.h"
#include "MediaPlayerPrivateHaiku.h"

#include <mutex>
#include <thread>

#include "MediaDecoderSelectionHaiku.h"
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#include "CoordinatedPlatformLayerBufferProxy.h"
#endif

#if ENABLE(VIDEO)

#include "GraphicsContext.h"
#include "NotImplemented.h"
#include "PlatformMediaResourceLoader.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "wtf/text/CString.h"
#include "wtf/NeverDestroyed.h"

#include <support/Autolock.h>
#include <support/Locker.h>
#include <Bitmap.h>
#include <DataIO.h>
#include <Entry.h>
#include <File.h>
#include <HttpRequest.h>
#include <MediaDefs.h>
#include <MediaFile.h>
#include <MediaFormats.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <SoundPlayer.h>
#include <View.h>
#include <OS.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

#if USE(CURL)
#include "StreamingMediaIOHaiku.h"
#include <cstring>
#include <curl/curl.h>
#endif


namespace WebCore {

static int mediaLifecycleTraceLevel()
{
    static int level = [] {
        const char* trace = std::getenv("SUMMIT_MEDIA_LIFECYCLE_TRACE");
        if (trace && trace[0] == '2' && !trace[1])
            return 2;
        return trace && trace[0] == '1' && !trace[1] ? 1 : 0;
    }();
    return level;
}

static bool mediaPlaybackTraceEnabled()
{
    static bool enabled = [] {
        const char* trace = std::getenv("SUMMIT_MEDIA_PLAYBACK_TRACE");
        return trace && trace[0] == '1' && !trace[1];
    }();
    return enabled;
}

static bool mediaFrameTraceEnabled()
{
    static bool enabled = [] {
        const char* trace = std::getenv("SUMMIT_MEDIA_FRAME_TRACE");
        return trace && trace[0] == '1' && !trace[1];
    }();
    return enabled;
}

class MediaPlayerFactoryHaiku final : public MediaPlayerFactory {
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::Haiku; };

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(* new MediaPlayerPrivate(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        return MediaPlayerPrivate::getSupportedTypes(types);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivate::supportsType(parameters);
    }

    bool supportsKeySystem(const String& keySystem, const String& mimeType) const final
    {
        return false;
        //return MediaPlayerPrivate::supportsKeySystem(keySystem, mimeType);
    }
};

void MediaPlayerPrivate::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryHaiku>());
}

MediaPlayerPrivate::MediaPlayerPrivate(MediaPlayer& player)
    : m_didReceiveData(false)
    , m_mediaFile(nullptr)
    , m_audioTrack(nullptr)
    , m_videoTrack(nullptr)
    , m_soundPlayer(nullptr)
    , m_frameBuffer(nullptr)
    , m_player(player)
    , m_networkState(MediaPlayer::NetworkState::Empty)
    , m_readyState(MediaPlayer::ReadyState::HaveNothing)
    , m_volume(1.0)
    , m_currentTime(0.f)
    , m_paused(true)
    , m_abort(false)
    , m_loadingThread(-1)
    , m_videoThread(-1)
{
}


MediaPlayerPrivate::~MediaPlayerPrivate()
{
    cancelLoad();
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivate::load(const URL& url, const LoadOptions&, WebCore::MediaSourcePrivateClient&)
{
    load(url.string());
}
#endif

// blob: and data: URLs name data that only WebCore can read (a blob lives in
// the network process); Media Kit's own URL reader cannot open them, and the
// media failed as "not supported". Pages make such media themselves: JW
// Player plays a tiny clip from a blob: URL to learn whether it may play at
// all, and on ABC News a player whose test failed took itself away again
// (issue #21). The whole resource is fetched through the page's media
// resource loader into a file that the loading thread opens like a download.
class MediaPrefetchClientHaiku final : public PlatformMediaResourceClient {
public:
    static Ref<MediaPrefetchClientHaiku> create(int descriptor, Function<void(bool)>&& completion)
    {
        return adoptRef(*new MediaPrefetchClientHaiku(descriptor, WTF::move(completion)));
    }

    ~MediaPrefetchClientHaiku()
    {
        if (m_descriptor >= 0)
            close(m_descriptor);
    }

    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        if (response.isInHTTPFamily() && response.httpStatusCode() >= 400)
            m_failed = true;
        completionHandler(ShouldContinuePolicyCheck::Yes);
    }

    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final
    {
        if (m_failed || m_descriptor < 0)
            return;
        buffer.forEachSegment([&](std::span<const uint8_t> segment) {
            while (!m_failed && !segment.empty()) {
                ssize_t written = write(m_descriptor, segment.data(), segment.size());
                if (written <= 0) {
                    m_failed = true;
                    break;
                }
                segment = segment.subspan(written);
            }
        });
    }

    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final { finish(false); }
    void loadFailed(PlatformMediaResource&, const ResourceError&) final { finish(false); }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final { finish(!m_failed); }

private:
    MediaPrefetchClientHaiku(int descriptor, Function<void(bool)>&& completion)
        : m_descriptor(descriptor)
        , m_completion(WTF::move(completion))
    {
    }

    void finish(bool succeeded)
    {
        if (m_descriptor >= 0) {
            if (close(m_descriptor))
                succeeded = false;
            m_descriptor = -1;
        }
        if (auto completion = std::exchange(m_completion, nullptr))
            completion(succeeded);
    }

    int m_descriptor;
    bool m_failed { false };
    Function<void(bool)> m_completion;
};

// Page thread, holding m_mediaLock.
bool MediaPlayerPrivate::startPrefetchHaiku(const URL& url)
{
    char name[] = "/tmp/summit-media-XXXXXX";
    int descriptor = mkstemp(name);
    if (descriptor < 0)
        return false;
    unsigned generation = ++m_prefetchGeneration;
    auto client = MediaPrefetchClientHaiku::create(descriptor, [weakThis = WeakPtr { *this }, generation](bool succeeded) {
        if (weakThis && weakThis->m_prefetchGeneration == generation)
            weakThis->didFinishPrefetchHaiku(succeeded);
    });
    Ref loader = m_player.mediaResourceLoader();
    m_prefetchResource = loader->requestResource(ResourceRequest { URL { url } }, PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!m_prefetchResource) {
        unlink(name);
        return false;
    }
    m_downloadPath = String::fromUTF8(name);
    m_prefetchResource->setClient(WTF::move(client));
    if (mediaPlaybackTraceEnabled())
        std::fprintf(stderr, "Summit media playback: player=%p fetching %s through WebCore\n", this, url.protocol().utf8().legacyCStringPointer());
    return true;
}

// Page thread.
void MediaPlayerPrivate::didFinishPrefetchHaiku(bool succeeded)
{
    if (RefPtr resource = std::exchange(m_prefetchResource, nullptr))
        resource->shutdown();
    if (mediaPlaybackTraceEnabled())
        std::fprintf(stderr, "Summit media playback: player=%p fetched=%d\n", this, succeeded);
    if (!succeeded || m_abort) {
        if (!m_abort) {
            m_networkState = MediaPlayer::NetworkState::NetworkError;
            m_player.networkStateChanged();
        }
        return;
    }
    m_mediaLock.Lock();
    m_prefetchedHaiku = true;
    m_loadingThread = spawn_thread(loadingThread, "media loader", B_NORMAL_PRIORITY, this);
    if (m_loadingThread >= 0)
        resume_thread(m_loadingThread);
    m_mediaLock.Unlock();
}

void MediaPlayerPrivate::load(const String& url)
{
    int traceLifecycle = mediaLifecycleTraceLevel();
    bigtime_t startedAt = traceLifecycle ? system_time() : 0;
    cancelLoad();
    bigtime_t cancelledAt = traceLifecycle ? system_time() : 0;

    m_mediaLock.Lock();
    m_abort = false;
    m_url = url;
    m_userAgent = m_player.userAgent().utf8().legacyCStringPointer();
    m_referrer = m_player.referrer().utf8().legacyCStringPointer();

    URL parsedURL { url };
    if (!((parsedURL.protocolIsBlob() || parsedURL.protocolIsData()) && startPrefetchHaiku(parsedURL))) {
        m_loadingThread = spawn_thread(loadingThread, "media loader", B_NORMAL_PRIORITY, this);
        if (m_loadingThread >= 0)
            resume_thread(m_loadingThread);
    }

    m_mediaLock.Unlock();

    if (traceLifecycle) {
        bigtime_t finishedAt = system_time();
        if (traceLifecycle > 1 || finishedAt - startedAt >= 10000) {
            std::fprintf(stderr,
                "Summit media lifecycle: player=%p operation=load total=%.1f cancel=%.1f startLoader=%.1f\n",
                this, (finishedAt - startedAt) / 1000.0, (cancelledAt - startedAt) / 1000.0,
                (finishedAt - cancelledAt) / 1000.0);
        }
    }
}


void MediaPlayerPrivate::cancelLoad()
{
    int traceLifecycle = mediaLifecycleTraceLevel();
    bigtime_t startedAt = traceLifecycle ? system_time() : 0;
    bool hadLoadingThread = m_loadingThread >= 0;
    bool hadVideoThread = false;
    bool hadSoundPlayer = false;
    m_abort = true;
    m_mediaTracksReady = false;
    // A fetch still under way finishes into nothing.
    ++m_prefetchGeneration;
    m_prefetchedHaiku = false;
    if (RefPtr resource = std::exchange(m_prefetchResource, nullptr))
        resource->shutdown();
#if USE(CURL)
    // curl_multi_wakeup() is thread-safe. Serialize the handle lifetime so a
    // cancellation cannot race the loader's curl_multi_cleanup().
    m_downloadLock.Lock();
    if (m_activeDownload)
        curl_multi_wakeup(static_cast<CURLM*>(m_activeDownload));
    m_downloadLock.Unlock();
#endif

    // Neither the decoder thread nor the sound callback can finish while the
    // caller holds m_mediaLock. Join and stop them before taking it.
    if (m_loadingThread >= 0) {
        status_t status;
        wait_for_thread(m_loadingThread, &status);
        m_loadingThread = -1;
    }
    bigtime_t loadingJoinedAt = traceLifecycle ? system_time() : 0;

    hadVideoThread = m_videoThread >= 0;
    if (m_videoThread >= 0) {
        status_t status;
        wait_for_thread(m_videoThread, &status);
        m_videoThread = -1;
    }
    ++m_seekGeneration;
    if (m_seekPromise) {
        m_seekPromise->reject(PlatformMediaError::Cancelled);
        m_seekPromise.reset();
    }
    bigtime_t videoJoinedAt = traceLifecycle ? system_time() : 0;

    // The blocking form sleeps for the output latency even after playback has
    // stopped. Destruction below releases the node after the threads have exited.
    hadSoundPlayer = m_soundPlayer;
    if (m_soundPlayer)
        m_soundPlayer->Stop(false);
    bigtime_t audioStoppedAt = traceLifecycle ? system_time() : 0;

    m_mediaLock.Lock();
    delete m_soundPlayer;
    m_soundPlayer = nullptr;

    m_substituteVideo = nullptr;
    delete m_mediaFile;
    m_mediaFile = nullptr;
    delete m_secondaryMediaFile;
    m_secondaryMediaFile = nullptr;
    delete m_downloadFile;
    m_downloadFile = nullptr;
    delete m_secondaryDownloadFile;
    m_secondaryDownloadFile = nullptr;
    m_audioTrack = nullptr;
    m_videoTrack = nullptr;
    m_videoTrackFile = nullptr;
    m_videoTrackIndex = -1;
    m_audioFormat = { };
    m_audioAtEnd = false;
    m_videoAtEnd = false;
    m_prerollTarget = -1;
    m_prerollFrames = 0;
    m_hasVideo = false;
    delete m_frameBuffer;
    m_frameBuffer = nullptr;
    m_currentTime = 0;
    m_duration = 0;
    m_buffered.clear();
    m_ended = false;
    if (!m_downloadPath.isEmpty()) {
        unlink(m_downloadPath.utf8().legacyCStringPointer());
        m_downloadPath = { };
    }
    if (!m_secondaryDownloadPath.isEmpty()) {
        unlink(m_secondaryDownloadPath.utf8().legacyCStringPointer());
        m_secondaryDownloadPath = { };
    }
    m_mediaLock.Unlock();

    if (traceLifecycle) {
        bigtime_t finishedAt = system_time();
        if (traceLifecycle > 1 || finishedAt - startedAt >= 10000) {
            std::fprintf(stderr,
                "Summit media lifecycle: player=%p operation=cancelLoad total=%.1f waitLoader=%.1f waitVideo=%.1f stopAudio=%.1f cleanup=%.1f hadLoader=%d hadVideo=%d hadAudio=%d\n",
                this, (finishedAt - startedAt) / 1000.0, (loadingJoinedAt - startedAt) / 1000.0,
                (videoJoinedAt - loadingJoinedAt) / 1000.0, (audioStoppedAt - videoJoinedAt) / 1000.0,
                (finishedAt - audioStoppedAt) / 1000.0, hadLoadingThread, hadVideoThread, hadSoundPlayer);
        }
    }
}


void MediaPlayerPrivate::prepareToPlay()
{
    // TODO should we seek the tracks to 0? reset m_currentTime? other things?
}

void MediaPlayerPrivate::playCallback(void* cookie, void* buffer,
    size_t /*size*/, const media_raw_audio_format& /*format*/)
{
    MediaPlayerPrivate* player = (MediaPlayerPrivate*)cookie;

    if (!player->m_mediaLock.Lock())
        return;

    if (player->m_audioTrack && !player->m_audioAtEnd) {
        // BMediaTrack::CurrentTime() counts decoded audio rather than the time
        // heard by the output device. It advances far too slowly on some MP4s;
        // the video thread supplies the presentation clock when there is video.
        if (!player->m_hasVideo)
            player->m_currentTime = player->m_audioTrack->CurrentTime() / 1000000.f;

        int64 size64;
        if (player->m_audioTrack->ReadFrames(buffer, &size64) != B_OK)
        {
            if (!player->m_hasVideo)
                player->m_currentTime = player->m_audioTrack->Duration() / 1000000.f;
            if (player->m_soundPlayer)
                player->m_soundPlayer->Stop(false);

            player->m_audioAtEnd = true;
            if ((!player->m_videoTrack || player->m_videoAtEnd) && !player->m_ended.exchange(true)) {
                player->m_currentTime = player->m_duration.load();
                callOnMainThread([p = WeakPtr(player)] {
                    if (p && p->m_ended) {
                        if (!p->m_player.isLooping())
                            p->pause();
                        p->m_player.timeChanged();
                        p->m_player.playbackStateChanged();
                    }
                });
            }
        }
    }
    player->m_mediaLock.Unlock();
}


status_t MediaPlayerPrivate::loadingThread(void* cookie)
{
    MediaPlayerPrivate* player = (MediaPlayerPrivate*)cookie;
    player->IdentifyTracks(player->m_url);

    if (mediaPlaybackTraceEnabled())
        std::fprintf(stderr, "Summit media playback: player=%p loaded file=%d video=%d audio=%d abort=%d duration=%.2f\n",
            player, player->m_mediaFile && player->m_mediaFile->InitCheck() == B_OK,
            bool(player->m_hasVideo), !!player->m_audioTrack, bool(player->m_abort), player->m_duration.load());

    if (player->m_abort)
        return B_OK;

    callOnMainThread([player = WeakPtr(player)] {
        if (!player)
            return;

        // Media Kit can recognize a container while rejecting every track.
        // Reporting that as loaded leaves HTMLMediaElement playing forever at
        // time zero and prevents a following <source> from being considered.
        if (player->m_mediaFile && player->m_mediaFile->InitCheck() == B_OK
            && (player->m_hasVideo || player->m_audioTrack)) {
            // Reported as buffered in full: a streamed file fetches whatever
            // Media Kit reads next, so playback waits for bytes, not for the
            // page. Downloaded files (HLS renditions) and local files are
            // complete already. Publish this on the page thread before media
            // events run.
            if (auto duration = player->duration(); duration > MediaTime::zeroTime())
                player->m_buffered.add(MediaTime::zeroTime(), duration);
            player->m_player.characteristicChanged();
            player->m_player.durationChanged();
            player->m_player.sizeChanged();
            player->m_player.firstVideoFrameAvailable();

            player->m_readyState = MediaPlayer::ReadyState::HaveEnoughData;
            player->m_networkState = MediaPlayer::NetworkState::Loaded;
            player->m_mediaTracksReady = true;
            player->startVideoPlaybackIfNeeded();
            player->startAudioPlaybackIfNeeded();
        } else {
            player->m_readyState = MediaPlayer::ReadyState::HaveMetadata;
            player->m_networkState = MediaPlayer::NetworkState::FormatError;
        }
        player->m_player.networkStateChanged();
        player->m_player.readyStateChanged();
    });

    return B_OK;
}


// Haiku's ffmpeg decoder reports a video frame's start_time in the stream's
// time base instead of microseconds (512 per frame for a 1/15360 MP4 at 30
// fps), and BMediaTrack::CurrentTime() adopts it from the first decoded frame
// on: every frame looked late and a 10 s clip played in half a second, and
// seek preroll could not reach its target. The frame counter (a seek sets it
// from the extractor, each decoded frame adds one) gives the time then. Only
// a decoder time far behind the counter is that bug: one ahead of it is a
// decoder that skips frames it does not deliver (NVDEC on the X399 delivers
// about a quarter of them, with real times), and there the counter would
// end a 10 s clip after 2.5 s.
static bigtime_t videoTrackTime(BMediaTrack& track)
{
    bigtime_t reported = track.CurrentTime();
    media_format format;
    if (track.EncodedFormat(&format) != B_OK)
        return reported;
    double rate = format.u.encoded_video.output.field_rate;
    if (!(rate > 0) || !std::isfinite(rate))
        return reported;
    auto counted = static_cast<bigtime_t>(track.CurrentFrame() * 1000000.0 / rate);
    auto tolerance = std::max(static_cast<bigtime_t>(2000000.0 / rate), counted / 10);
    return reported + tolerance < counted ? counted : reported;
}

status_t MediaPlayerPrivate::videoThread(void* cookie)
{
    MediaPlayerPrivate* player = (MediaPlayerPrivate*)cookie;
    bigtime_t startTime = system_time();
    bigtime_t startMediaTime = 0;
    float lastObservedTime = 0;
    unsigned decodedFrames = 0;
    int lastTraceSecond = -1;
    const bool tracePlayback = mediaPlaybackTraceEnabled();
    bigtime_t pausedSince = 0;
    // The track's own decoder, or the one that decodes in its place.
    auto readVideoFrame = [player](void* buffer, int64* count, media_header* header) {
        if (player->m_substituteVideo)
            return player->m_substituteVideo->readFrame(buffer, count, header);
        return player->m_videoTrack->ReadFrames(buffer, count, header);
    };
    auto decodedVideoTime = [player] {
        return player->m_substituteVideo ? player->m_substituteVideo->currentTime() : videoTrackTime(*player->m_videoTrack);
    };

    while (!player->m_abort) {
        if (player->m_paused && player->m_prerollTarget < 0) {
            if (!pausedSince)
                pausedSince = system_time();
            // An offscreen autoplay video can remain paused for the rest of a
            // feed visit. Release its decoder on this thread so the page does
            // not wait for Media Kit, while keeping the last painted frame.
            if (system_time() - pausedSince >= 2000000 && !player->m_videoSeekStarting
                && player->m_mediaLock.Lock()) {
                if (!player->m_abort && player->m_paused && player->m_prerollTarget < 0
                    && !player->m_videoSeekStarting && player->m_videoTrack) {
                    bigtime_t releaseStartedAt = tracePlayback ? system_time() : 0;
                    player->m_substituteVideo = nullptr;
                    player->m_videoTrackFile->ReleaseTrack(player->m_videoTrack);
                    player->m_videoTrack = nullptr;
                    if (tracePlayback)
                        std::fprintf(stderr, "Summit media playback: player=%p videoDecoder=park-paused index=%d release=%.1f\n",
                            player, player->m_videoTrackIndex, (system_time() - releaseStartedAt) / 1000.0);
                    player->m_mediaLock.Unlock();
                    return B_OK;
                }
                player->m_mediaLock.Unlock();
            }
            snooze(10000);
            startTime = system_time();
            startMediaTime = (bigtime_t)(player->m_currentTime * 1000000);
            lastObservedTime = player->m_currentTime;
            continue;
        }
        pausedSince = 0;

        if (!player->m_mediaLock.Lock())
            continue;

        // Decode keyframe preroll away from the page thread. Release the media
        // lock after each frame so another drag position can supersede this
        // seek and the audio callback can continue running.
        bigtime_t prerollTarget = player->m_prerollTarget;
        if (prerollTarget >= 0 && player->m_videoTrack && player->m_frameBuffer) {
            int64 count = 1;
            status_t status = readVideoFrame(player->m_frameBuffer->Bits(), &count, nullptr);
            if (status == B_OK)
                ++player->m_prerollFrames;
            if (status != B_OK || decodedVideoTime() >= prerollTarget || player->m_prerollFrames >= 300) {
                unsigned generation = player->m_seekGeneration;
                if (tracePlayback)
                    std::fprintf(stderr, "Summit media seek: player=%p requested=%.3f keyframe=%.3f decoded=%u position=%.3f\n",
                        player, prerollTarget / 1000000.0, player->m_prerollKeyframeTime / 1000000.0,
                        player->m_prerollFrames, decodedVideoTime() / 1000000.0);
                player->m_prerollTarget = -1;
                player->pushFrameToCompositor();
                startTime = system_time();
                startMediaTime = (bigtime_t)(player->m_currentTime * 1000000);
                lastObservedTime = player->m_currentTime;
                callOnMainThread([player = WeakPtr(player), generation] {
                    if (!player || player->m_seekGeneration != generation || !player->m_seekPromise)
                        return;
                    player->m_player.repaint();
                    auto promise = std::exchange(player->m_seekPromise, std::nullopt);
                    promise->resolve(player->currentTime());
                    player->m_player.timeChanged();
                });
            }
            player->m_mediaLock.Unlock();
            snooze(1000);
            continue;
        }

        bool waitForFrame = false;
        if (player->m_videoTrack && !player->m_videoAtEnd) {
            // Detect seek or external time update
            if (std::abs(player->m_currentTime - lastObservedTime) > 0.5f) {
                startTime = system_time();
                startMediaTime = (bigtime_t)(player->m_currentTime * 1000000);
            }

            bigtime_t videoTime = decodedVideoTime();
            bigtime_t playbackTime = startMediaTime + (system_time() - startTime);

            player->m_currentTime = playbackTime / 1000000.f;
            lastObservedTime = player->m_currentTime;

            if (videoTime < playbackTime) {
                int64 count;
                bigtime_t decodeStartedAt = mediaFrameTraceEnabled() ? system_time() : 0;
                media_header header = { };
                status_t decodeStatus = readVideoFrame(player->m_frameBuffer->Bits(), &count, &header);
                if (tracePlayback && decodeStatus == B_OK && (decodedFrames < 8 || !(decodedFrames % 10)))
                    std::fprintf(stderr, "Summit media frame: player=%p frame=%u count=%lld start=%lld current=%lld playback=%lld\n",
                        player, decodedFrames, static_cast<long long>(count), static_cast<long long>(header.start_time),
                        static_cast<long long>(decodedVideoTime()), static_cast<long long>(playbackTime));
                if (decodeStatus == B_OK) {
                    if (mediaFrameTraceEnabled()) {
                        bigtime_t decodedAt = system_time();
                        player->m_lastFrameTraceDecodedAt = decodedAt;
                        player->m_lastFrameTraceDecodeDuration = decodedAt - decodeStartedAt;
                        ++player->m_frameTraceSequence;
                    }
                    if (tracePlayback) {
                        ++decodedFrames;
                        int second = static_cast<int>(player->m_currentTime.load());
                        if (lastTraceSecond < 0 || second >= lastTraceSecond + 5) {
                            lastTraceSecond = second;
                            std::fprintf(stderr, "Summit media playback: player=%p videoFrames=%u time=%.2f duration=%.2f\n",
                                player, decodedFrames, player->m_currentTime.load(), player->m_duration.load());
                        }
                    }
                    ++player->m_videoFrameGeneration;
                    if (!player->pushFrameToCompositor())
                        player->queueVideoFrameRepaint();
                } else {
                    if (tracePlayback)
                        std::fprintf(stderr, "Summit media playback: player=%p videoEnd status=%ld frames=%u time=%.2f duration=%.2f\n",
                            player, long(decodeStatus), decodedFrames, player->m_currentTime.load(), player->m_duration.load());
                    player->m_videoAtEnd = true;
                    if ((!player->m_audioTrack || player->m_audioAtEnd) && !player->m_ended.exchange(true)) {
                        player->m_currentTime = player->m_duration.load();
                        callOnMainThread([player = WeakPtr(player)] {
                            if (player && player->m_ended) {
                                if (!player->m_player.isLooping())
                                    player->pause();
                                player->m_player.timeChanged();
                                player->m_player.playbackStateChanged();
                            }
                        });
                    }
                }
            } else
                waitForFrame = true;
        } else if (player->m_videoAtEnd)
            waitForFrame = true;
        player->m_mediaLock.Unlock();
        // The audio callback and seeks need this lock. Waiting for the next
        // video frame while holding it can stall audio on every frame.
        snooze(waitForFrame ? 5000 : 1000);
    }

    return B_OK;
}

bool MediaPlayerPrivate::pushFrameToCompositor()
{
#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
    RefPtr<CoordinatedPlatformLayerBufferProxy> proxy;
    {
        BAutolock locker(m_layerBufferProxyLock);
        proxy = m_layerBufferProxy;
    }
    if (!proxy || !proxy->isValid() || !m_frameBuffer)
        return false;
    // The decoder writes the next picture into the same bitmap, so the
    // compositor gets its own copy. Allocating that copy for every frame
    // (8 MB at 1080p) had the kernel fault in and zero fresh pages each time;
    // the buffers are reused once the compositor has let go of them, as the
    // MSE player does.
    BRect bounds = m_frameBuffer->Bounds();
    auto info = SkImageInfo::Make(bounds.IntegerWidth() + 1, bounds.IntegerHeight() + 1,
        kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
    size_t rowBytes = m_frameBuffer->BytesPerRow();
    size_t size = rowBytes * info.height();
    sk_sp<SkData> pixels;
    for (auto& buffer : m_compositorBuffers) {
        if (buffer->unique() && buffer->size() == size) {
            pixels = buffer;
            break;
        }
    }
    if (!pixels) {
        m_compositorBuffers.removeAllMatching([](auto& buffer) { return buffer->unique(); });
        pixels = SkData::MakeUninitialized(size);
        if (m_compositorBuffers.size() < 6)
            m_compositorBuffers.append(pixels);
    }
    std::memcpy(pixels->writable_data(), m_frameBuffer->Bits(), size);
    auto image = SkImages::RasterFromData(info, WTF::move(pixels), rowBytes);
    if (!image)
        return false;
    RefPtr nativeImage = NativeImage::create(WTF::move(image));
    if (!nativeImage)
        return false;
    proxy->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(nativeImage.releaseNonNull(), nullptr));
    return true;
#else
    return false;
#endif
}

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
void MediaPlayerPrivate::setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&& proxy)
{
    {
        BAutolock locker(m_layerBufferProxyLock);
        m_layerBufferProxy = proxy.copyRef();
    }
    if (m_mediaLock.Lock()) {
        pushFrameToCompositor();
        m_mediaLock.Unlock();
    }
}

RefPtr<CoordinatedPlatformLayerBufferProxy> MediaPlayerPrivate::platformLayerBufferProxy() const
{
    BAutolock locker(m_layerBufferProxyLock);
    return m_layerBufferProxy;
}
#endif

void MediaPlayerPrivate::queueVideoFrameRepaint()
{
    // The decoder can keep advancing while page script blocks the main thread.
    // Only the newest frame is in m_frameBuffer, so older queued repaint calls
    // cannot display their original frames and just delay the current one.
    if (m_videoRepaintQueued.exchange(true))
        return;

    if (mediaFrameTraceEnabled()) {
        m_frameTraceRepaintQueuedAt = system_time();
        m_frameTraceRepaintQueuedSequence = m_frameTraceSequence.load();
    }

    callOnMainThread([player = WeakPtr(this)] {
        if (!player)
            return;
        unsigned generation = player->m_videoFrameGeneration;
        if (mediaFrameTraceEnabled()) {
            unsigned sequence = player->m_frameTraceSequence;
            bigtime_t decodedAt = player->m_lastFrameTraceDecodedAt;
            bigtime_t now = system_time();
            std::fprintf(stderr, "Summit media frame: player=%p seq=%u decode=%.1f queue=%.1f latest=%.1f skipped=%u media=%.3f at=%lld\n",
                player.get(), sequence, player->m_lastFrameTraceDecodeDuration.load() / 1000.0,
                (now - player->m_frameTraceRepaintQueuedAt.load()) / 1000.0,
                (now - decodedAt) / 1000.0,
                sequence - player->m_frameTraceRepaintQueuedSequence.load(),
                player->m_currentTime.load(), static_cast<long long>(now));
        }
        player->m_player.repaint();
        player->m_videoRepaintQueued = false;
        // A decode that arrived during repaint needs its own invalidation.
        // Decodes after this check see the cleared flag and enqueue it.
        if (player->m_videoFrameGeneration != generation)
            player->queueVideoFrameRepaint();
    });
}




void MediaPlayerPrivate::startVideoPlaybackIfNeeded(bool forSeek)
{
    if (!m_mediaTracksReady || !m_hasVideo || (m_paused && !forSeek)
        || !m_videoTrackFile || m_videoTrackIndex < 0)
        return;

    if (!m_mediaLock.Lock())
        return;
    if (m_videoTrack) {
        m_mediaLock.Unlock();
        return;
    }
    thread_id parkedThread = m_videoThread;
    m_mediaLock.Unlock();
    if (parkedThread >= 0) {
        // A paused decoder leaves its thread ID for the owner to reap. It
        // clears m_videoTrack only immediately before returning.
        status_t status;
        wait_for_thread(parkedThread, &status);
        m_videoThread = -1;
    }
    if (!m_mediaLock.Lock())
        return;
    if (m_videoTrack) {
        m_mediaLock.Unlock();
        return;
    }

    BMediaTrack* track = m_videoTrackFile->TrackAt(m_videoTrackIndex);
    media_format format = { };
    format.type = B_MEDIA_RAW_VIDEO;
    format.u.raw_video.display.format = B_RGB32;
    std::unique_ptr<SubstituteVideoDecoderHaiku> fallbackVideo;
    status_t decodeStatus = track ? track->DecodedFormat(&format) : B_ERROR;
    if (track && decodeStatus != B_OK) {
        fallbackVideo = SubstituteVideoDecoderHaiku::createForTrackWithoutDecoder(*track, format);
        if (fallbackVideo) {
            format = fallbackVideo->decodedFormat();
            decodeStatus = B_OK;
        }
    }
    if (!track || decodeStatus != B_OK) {
        if (track)
            m_videoTrackFile->ReleaseTrack(track);
        m_networkState = MediaPlayer::NetworkState::FormatError;
        m_mediaLock.Unlock();
        m_player.networkStateChanged();
        return;
    }

    m_substituteVideo = fallbackVideo ? WTF::move(fallbackVideo) : SubstituteVideoDecoderHaiku::createIfNeeded(*track, format);
    if (m_substituteVideo)
        format = m_substituteVideo->decodedFormat();
    if (!m_frameBuffer || static_cast<uint32>(m_frameBuffer->Bounds().IntegerWidth() + 1) != format.Width()
        || static_cast<uint32>(m_frameBuffer->Bounds().IntegerHeight() + 1) != format.Height()) {
        delete m_frameBuffer;
        m_frameBuffer = new BBitmap(BRect(0, 0, format.Width() - 1, format.Height() - 1), B_RGB32);
    }
    // A track taken again from the same file continues from where the
    // released one left its stream, not from the start: at first play (time
    // zero, parked since loading) the decoder began mid-stream, missed its
    // reference frames and reported the end after a fraction of a second.
    // Always position a resumed track at the current time.
    if (!forSeek) {
        bigtime_t target = static_cast<bigtime_t>(std::max(0.f, m_currentTime.load()) * 1000000);
        bigtime_t keyframe = target;
        if ((m_substituteVideo ? m_substituteVideo->seekToTime(&keyframe) : track->SeekToTime(&keyframe)) == B_OK) {
            m_prerollKeyframeTime = keyframe;
            m_prerollFrames = 0;
            m_prerollTarget = target;
        }
    }
    m_videoTrack = track;
    m_videoThread = spawn_thread(videoThread, "video decoder", B_NORMAL_PRIORITY, this);
    if (m_videoThread < 0) {
        m_substituteVideo = nullptr;
        m_videoTrackFile->ReleaseTrack(m_videoTrack);
        m_videoTrack = nullptr;
        m_prerollTarget = -1;
        m_networkState = MediaPlayer::NetworkState::FormatError;
        m_mediaLock.Unlock();
        m_player.networkStateChanged();
        return;
    }
    if (mediaPlaybackTraceEnabled())
        std::fprintf(stderr, "Summit media playback: player=%p videoDecoder=resume index=%d\n",
            this, m_videoTrackIndex);
    resume_thread(m_videoThread);
    m_mediaLock.Unlock();
}

void MediaPlayerPrivate::startAudioPlaybackIfNeeded()
{
    if (m_paused || m_muted || !m_mediaTracksReady || !m_audioTrack)
        return;
    if (!m_soundPlayer) {
        // A muted or paused clip does not advance its audio track. Match the
        // video's clock when sound is enabled later in the playback.
        if (m_mediaLock.Lock()) {
            bigtime_t audioTime = m_audioTrack->CurrentTime();
            bigtime_t playbackTime = (bigtime_t)(m_currentTime.load() * 1000000);
            if (std::abs(audioTime - playbackTime) > 100000)
                m_audioTrack->SeekToTime(&playbackTime);
            m_mediaLock.Unlock();
        }
        m_soundPlayer = new BSoundPlayer(&m_audioFormat, "HTML5 Audio", playCallback, NULL, this);
        status_t status = m_soundPlayer->InitCheck();
        if (mediaPlaybackTraceEnabled())
            std::fprintf(stderr, "Summit media playback: player=%p audioOutput=create status=%ld\n", this, long(status));
        if (status != B_OK) {
            delete m_soundPlayer;
            m_soundPlayer = nullptr;
            return;
        }
        m_soundPlayer->SetVolume(m_volume);
    }
    m_soundPlayer->Start();
}

void MediaPlayerPrivate::releaseAudioOutput()
{
    if (m_soundPlayer) {
        auto* soundPlayer = m_soundPlayer;
        soundPlayer->Stop(false);
        m_mediaLock.Lock();
        m_soundPlayer = nullptr;
        m_mediaLock.Unlock();
        delete soundPlayer;
        if (mediaPlaybackTraceEnabled())
            std::fprintf(stderr, "Summit media playback: player=%p audioOutput=release\n", this);
    }
}

void MediaPlayerPrivate::play()
{
    m_paused = false;
    startVideoPlaybackIfNeeded();
    startAudioPlaybackIfNeeded();
}

void MediaPlayerPrivate::pause()
{
    m_paused = true;
    releaseAudioOutput();
}

FloatSize MediaPlayerPrivate::naturalSize() const
{
    if (!m_frameBuffer)
        return FloatSize(0,0);

    BRect r(m_frameBuffer->Bounds());
    return FloatSize(r.Width() + 1, r.Height() + 1);
}

bool MediaPlayerPrivate::hasAudio() const
{
    return m_audioTrack;
}

bool MediaPlayerPrivate::hasVideo() const
{
    return m_hasVideo;
}

void MediaPlayerPrivate::setPageIsVisible(bool)
{
    notImplemented();
}

WTF::MediaTime MediaPlayerPrivate::duration() const
{
    return WTF::MediaTime::createWithDouble(m_duration);
}

WTF::MediaTime MediaPlayerPrivate::currentTime() const
{
    float duration = m_duration;
    float currentTime = m_currentTime;
    return WTF::MediaTime::createWithDouble(duration > 0 ? std::min(currentTime, duration) : currentTime);
}

Ref<MediaTimePromise> MediaPlayerPrivate::seekToTarget(const SeekTarget& time)
{
    m_videoSeekStarting = true;
    startVideoPlaybackIfNeeded(true);
    if (!m_mediaLock.Lock()) {
        m_videoSeekStarting = false;
        return MediaTimePromise::createAndReject(PlatformMediaError::InvalidState);
    }
    if (auto previousSeek = std::exchange(m_seekPromise, std::nullopt))
        previousSeek->reject(PlatformMediaError::Cancelled);
    ++m_seekGeneration;
    if (!m_videoTrack && !m_audioTrack) {
        m_videoSeekStarting = false;
        m_mediaLock.Unlock();
        return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
    }
    if (m_hasVideo && (!m_videoTrack || m_videoThread < 0 || !m_frameBuffer)) {
        m_videoSeekStarting = false;
        m_mediaLock.Unlock();
        return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
    }
    bigtime_t requestedTime = (bigtime_t)(time.time.toDouble() * 1000000);
    bigtime_t videoTime = requestedTime;
    status_t status = B_OK;
    if (m_videoTrack)
        status = m_substituteVideo ? m_substituteVideo->seekToTime(&videoTime) : m_videoTrack->SeekToTime(&videoTime);
    if (m_audioTrack && status == B_OK) {
        bigtime_t audioTime = requestedTime;
        status = m_audioTrack->SeekToTime(&audioTime);
    }
    if (status != B_OK) {
        m_videoSeekStarting = false;
        m_mediaLock.Unlock();
        return MediaTimePromise::createAndReject(PlatformMediaError::NotSupportedError);
    }

    bool restartAudio = m_audioAtEnd && !m_paused;
    m_audioAtEnd = false;
    m_videoAtEnd = false;
    m_currentTime = requestedTime / 1000000.f;
    m_ended = false;
    bool prerollVideo = m_videoTrack;
    if (prerollVideo) {
        m_prerollKeyframeTime = videoTime;
        m_prerollFrames = 0;
        m_prerollTarget = requestedTime;
    }
    auto position = currentTime();
    m_videoSeekStarting = false;
    m_mediaLock.Unlock();
    if (restartAudio)
        startAudioPlaybackIfNeeded();
    if (prerollVideo) {
        m_seekPromise.emplace(PlatformMediaError::Cancelled);
        return m_seekPromise->promise();
    }
    m_player.timeChanged();
    return MediaTimePromise::createAndResolve(position);
}


bool MediaPlayerPrivate::paused() const
{
    return m_paused;
}

void MediaPlayerPrivate::setVolume(float volume)
{
    m_volume = volume;
    if (m_soundPlayer)
        m_soundPlayer->SetVolume(volume);
}

void MediaPlayerPrivate::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    if (mediaPlaybackTraceEnabled())
        std::fprintf(stderr, "Summit media playback: player=%p muted=%d\n", this, muted);
    if (muted)
        releaseAudioOutput();
    else
        startAudioPlaybackIfNeeded();
}

MediaPlayer::NetworkState MediaPlayerPrivate::networkState() const
{
    return m_networkState;
}

MediaPlayer::ReadyState MediaPlayerPrivate::readyState() const
{
    return m_readyState;
}

const PlatformTimeRanges& MediaPlayerPrivate::buffered() const
{
    return m_buffered;
}

bool MediaPlayerPrivate::didLoadingProgress() const
{
    bool progress = m_didReceiveData;
    m_didReceiveData = false;
    return progress;
}

void MediaPlayerPrivate::paint(GraphicsContext& context, const FloatRect& r)
{
    if (context.paintingDisabled())
        return;

    if (!m_frameBuffer)
        return;
    if (mediaFrameTraceEnabled()) {
        unsigned sequence = m_frameTraceSequence;
        if (sequence && m_lastFrameTracePaintedSequence.exchange(sequence) != sequence)
            std::fprintf(stderr, "Summit media paint: player=%p seq=%u age=%.1f media=%.3f\n",
                this, sequence, (system_time() - m_lastFrameTraceDecodedAt.load()) / 1000.0, m_currentTime.load());
    }
#if USE(HAIKU_GRAPHICS)
    BView* target = context.platformContext();
    target->SetDrawingMode(B_OP_COPY);
    target->DrawBitmap(m_frameBuffer, r);
#else
    // The decoder still hands us a BBitmap. Borrow its pixels rather than copy
    // them: the draw happens before this returns, and the frame outlives it.
    // B_RGBA32 is BGRA with straight alpha.
    BRect bounds = m_frameBuffer->Bounds();
    auto info = SkImageInfo::Make(bounds.IntegerWidth() + 1, bounds.IntegerHeight() + 1,
        kBGRA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, m_frameBuffer->Bits(), m_frameBuffer->BytesPerRow());
    if (auto image = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr)) {
        if (auto frame = NativeImage::create(WTF::move(image)))
            context.drawNativeImage(*frame, r, FloatRect({ }, frame->size()));
    }
#endif
}

// #pragma mark - private methods

static bool canPlayAudio(const media_raw_audio_format& format)
{
    if (!(format.frame_rate > 0) || !format.channel_count || format.channel_count > 32 || !format.format)
        return false;
    // An audio output node exists only when the system has a sound device.
    BMediaRoster* roster = BMediaRoster::Roster();
    media_node output;
    if (!roster || roster->GetAudioOutput(&output) != B_OK)
        return false;
    roster->ReleaseNode(output);
    return true;
}

#if USE(CURL)
struct MediaDownloadCancellation {
    std::atomic<bool>& abort;
    BLocker& lock;
    void*& activeDownload;
};

static bool downloadMediaFile(const BUrl& url, MediaDownloadCancellation& cancellation, String& path)
{
    char name[] = "/tmp/summit-media-XXXXXX";
    int descriptor = mkstemp(name);
    if (descriptor < 0)
        return false;
    FILE* output = fdopen(descriptor, "wb");
    if (!output) {
        close(descriptor);
        unlink(name);
        return false;
    }

    struct Download {
        FILE* output;
        std::atomic<bool>& abort;
        size_t bytes { 0 };
    } download { output, cancellation.abort };

    CURL* request = curl_easy_init();
    CURLcode result = CURLE_FAILED_INIT;
    long response = 0;
    if (request) {
        curl_easy_setopt(request, CURLOPT_URL, url.UrlString().String());
        curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(request, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(request, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(request, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(request, CURLOPT_XFERINFODATA, &download);
        curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<Download*>(context)->abort ? 1 : 0;
        });
        curl_easy_setopt(request, CURLOPT_WRITEDATA, &download);
        curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, +[](char* bytes, size_t size, size_t count, void* context) -> size_t {
            auto& download = *static_cast<Download*>(context);
            size_t length = size * count;
            constexpr size_t maxMediaBytes = 256 * 1024 * 1024;
            if (download.abort || length > maxMediaBytes - download.bytes)
                return 0;
            size_t written = fwrite(bytes, 1, length, download.output);
            download.bytes += written;
            return written;
        });
        CURLM* multi = curl_multi_init();
        if (multi && curl_multi_add_handle(multi, request) == CURLM_OK) {
            cancellation.lock.Lock();
            cancellation.activeDownload = multi;
            cancellation.lock.Unlock();

            int running = 0;
            CURLMcode multiResult = curl_multi_perform(multi, &running);
            while (multiResult == CURLM_OK && running && !cancellation.abort) {
                int descriptorCount = 0;
                multiResult = curl_multi_poll(multi, nullptr, 0, 1000, &descriptorCount);
                if (multiResult == CURLM_OK)
                    multiResult = curl_multi_perform(multi, &running);
            }

            if (cancellation.abort)
                result = CURLE_ABORTED_BY_CALLBACK;
            else if (multiResult == CURLM_OK) {
                int messagesRemaining = 0;
                while (CURLMsg* message = curl_multi_info_read(multi, &messagesRemaining)) {
                    if (message->msg == CURLMSG_DONE && message->easy_handle == request) {
                        result = message->data.result;
                        break;
                    }
                }
            }

            cancellation.lock.Lock();
            cancellation.activeDownload = nullptr;
            cancellation.lock.Unlock();
            curl_multi_remove_handle(multi, request);
        }
        if (multi)
            curl_multi_cleanup(multi);
        curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &response);
        curl_easy_cleanup(request);
    }
    bool closed = fclose(output) == 0;
    if (result != CURLE_OK || response < 200 || response >= 300 || !closed || !download.bytes || cancellation.abort) {
        unlink(name);
        return false;
    }
    path = String::fromUTF8(name);
    return true;
}

static bool mediaStreamingEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_MEDIA_STREAMING");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

static std::string_view trimPlaylistLine(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.remove_suffix(1);
    return line;
}

static std::vector<std::string_view> playlistLines(const std::string& playlist)
{
    std::vector<std::string_view> lines;
    std::string_view remaining(playlist);
    while (!remaining.empty()) {
        size_t end = remaining.find('\n');
        auto line = trimPlaylistLine(remaining.substr(0, end));
        if (!line.empty())
            lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    return lines;
}

static std::optional<std::string_view> playlistAttribute(std::string_view line, std::string_view name)
{
    size_t offset = 0;
    while ((offset = line.find(name, offset)) != std::string_view::npos) {
        bool startsAttribute = !offset || line[offset - 1] == ',' || line[offset - 1] == ':';
        size_t valueOffset = offset + name.size();
        if (!startsAttribute || valueOffset >= line.size() || line[valueOffset] != '=') {
            offset = valueOffset;
            continue;
        }
        ++valueOffset;
        if (valueOffset >= line.size())
            return std::nullopt;
        if (line[valueOffset] == '"') {
            size_t end = line.find('"', valueOffset + 1);
            if (end == std::string_view::npos)
                return std::nullopt;
            return line.substr(valueOffset + 1, end - valueOffset - 1);
        }
        size_t end = line.find(',', valueOffset);
        return line.substr(valueOffset, end == std::string_view::npos ? end : end - valueOffset);
    }
    return std::nullopt;
}

static bool downloadPlaylist(const BUrl& url, MediaDownloadCancellation& cancellation, std::string& playlist)
{
    String path;
    if (!downloadMediaFile(url, cancellation, path))
        return false;

    auto pathCString = path.utf8();
    FILE* input = fopen(pathCString.legacyCStringPointer(), "rb");
    bool succeeded = false;
    if (input && !fseek(input, 0, SEEK_END)) {
        long length = ftell(input);
        constexpr long maximumPlaylistBytes = 1024 * 1024;
        if (length > 0 && length <= maximumPlaylistBytes && !fseek(input, 0, SEEK_SET)) {
            playlist.resize(static_cast<size_t>(length));
            succeeded = fread(playlist.data(), 1, playlist.size(), input) == playlist.size();
        }
    }
    if (input)
        fclose(input);
    unlink(pathCString.legacyCStringPointer());
    return succeeded && !cancellation.abort;
}

static std::optional<BUrl> resolvePlaylistReference(const BUrl& base, std::string_view reference)
{
    std::string referenceString(reference);
    BUrl result(base, BString(referenceString.c_str()));
    if (!result.IsValid() || (result.Protocol() != "http" && result.Protocol() != "https"))
        return std::nullopt;
    return result;
}

struct HLSVariant {
    uint64_t bandwidth { 0 };
    std::string playlist;
    std::string audioGroup;
};

struct HLSAudioRendition {
    std::string group;
    std::string playlist;
};

static bool selectHLSVariant(const std::string& masterPlaylist, HLSVariant& selected,
    std::optional<HLSAudioRendition>& selectedAudio)
{
    auto lines = playlistLines(masterPlaylist);
    if (lines.empty() || lines[0] != "#EXTM3U")
        return false;
    std::vector<HLSAudioRendition> audioRenditions;
    std::vector<HLSVariant> variants;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto line = lines[i];
        if (line.starts_with("#EXT-X-MEDIA:")) {
            auto type = playlistAttribute(line, "TYPE");
            auto group = playlistAttribute(line, "GROUP-ID");
            auto uri = playlistAttribute(line, "URI");
            if (type && *type == "AUDIO" && group && uri)
                audioRenditions.push_back({ std::string(*group), std::string(*uri) });
            continue;
        }
        if (!line.starts_with("#EXT-X-STREAM-INF:"))
            continue;
        size_t uriIndex = i + 1;
        if (uriIndex == lines.size() || lines[uriIndex].starts_with('#'))
            continue;
        HLSVariant variant;
        variant.playlist = lines[uriIndex];
        if (auto bandwidth = playlistAttribute(line, "BANDWIDTH")) {
            std::string value(*bandwidth);
            variant.bandwidth = strtoull(value.c_str(), nullptr, 10);
        }
        if (auto audioGroup = playlistAttribute(line, "AUDIO"))
            variant.audioGroup = *audioGroup;
        variants.push_back(WTF::move(variant));
    }
    if (variants.empty())
        return false;

    selected = *std::max_element(variants.begin(), variants.end(), [](const auto& a, const auto& b) {
        return a.bandwidth < b.bandwidth;
    });
    if (!selected.audioGroup.empty()) {
        auto rendition = std::find_if(audioRenditions.begin(), audioRenditions.end(), [&](const auto& candidate) {
            return candidate.group == selected.audioGroup;
        });
        if (rendition == audioRenditions.end())
            return false;
        selectedAudio = *rendition;
    }
    return true;
}

static std::optional<BUrl> mediaURLForByteRangePlaylist(const BUrl& playlistURL,
    MediaDownloadCancellation& cancellation)
{
    std::string playlist;
    if (!downloadPlaylist(playlistURL, cancellation, playlist))
        return std::nullopt;

    auto lines = playlistLines(playlist);
    if (lines.empty() || lines[0] != "#EXTM3U")
        return std::nullopt;
    std::optional<BUrl> mediaURL;
    bool hasMap = false;
    bool hasSegment = false;
    bool hasEndList = false;
    bool nextSegmentHasByteRange = false;
    for (auto line : lines) {
        if (line.starts_with("#EXT-X-KEY:") || line == "#EXT-X-DISCONTINUITY")
            return std::nullopt;
        std::optional<std::string_view> reference;
        if (line.starts_with("#EXT-X-MAP:")) {
            reference = playlistAttribute(line, "URI");
            hasMap = reference.has_value() && playlistAttribute(line, "BYTERANGE").has_value();
            if (!hasMap)
                return std::nullopt;
        } else if (line.starts_with("#EXT-X-BYTERANGE:")) {
            if (nextSegmentHasByteRange)
                return std::nullopt;
            nextSegmentHasByteRange = true;
            continue;
        } else if (line == "#EXT-X-ENDLIST") {
            hasEndList = true;
            continue;
        } else if (!line.starts_with('#')) {
            if (!nextSegmentHasByteRange)
                return std::nullopt;
            nextSegmentHasByteRange = false;
            hasSegment = true;
            reference = line;
        }
        if (!reference)
            continue;
        auto resolved = resolvePlaylistReference(playlistURL, *reference);
        if (!resolved)
            return std::nullopt;
        if (mediaURL && mediaURL->UrlString() != resolved->UrlString())
            return std::nullopt;
        mediaURL = WTF::move(*resolved);
    }
    if (!hasMap || !hasSegment || !hasEndList || nextSegmentHasByteRange)
        return std::nullopt;
    return mediaURL;
}

static bool downloadHLSByteRangeMedia(const BUrl& masterURL, MediaDownloadCancellation& cancellation,
    String& videoPath, String& audioPath)
{
    std::string masterPlaylist;
    if (!downloadPlaylist(masterURL, cancellation, masterPlaylist))
        return false;
    HLSVariant variant;
    std::optional<HLSAudioRendition> audioRendition;
    if (!selectHLSVariant(masterPlaylist, variant, audioRendition))
        return false;

    auto videoPlaylistURL = resolvePlaylistReference(masterURL, variant.playlist);
    if (!videoPlaylistURL)
        return false;
    auto videoURL = mediaURLForByteRangePlaylist(*videoPlaylistURL, cancellation);
    if (!videoURL || !downloadMediaFile(*videoURL, cancellation, videoPath))
        return false;

    if (!audioRendition)
        return true;
    auto audioPlaylistURL = resolvePlaylistReference(masterURL, audioRendition->playlist);
    auto audioURL = audioPlaylistURL ? mediaURLForByteRangePlaylist(*audioPlaylistURL, cancellation) : std::nullopt;
    if (!audioURL || !downloadMediaFile(*audioURL, cancellation, audioPath)) {
        auto videoPathCString = videoPath.utf8();
        unlink(videoPathCString.legacyCStringPointer());
        videoPath = { };
        return false;
    }
    if (const char* trace = std::getenv("SUMMIT_MEDIA_CODEC_TRACE"); trace && trace[0] == '1' && !trace[1])
        std::fprintf(stderr, "Summit media HLS: resolved finite byte-range CMAF video and audio\n");
    return true;
}

static bool isHLSPlaylistURL(const BUrl& url)
{
    BString urlPath = url.Path();
    std::string_view path(urlPath.String());
    return path.ends_with(".m3u8") || path.ends_with(".M3U8");
}
#endif

void MediaPlayerPrivate::IdentifyTracks(const String& url)
{
    // Use Media Kit's file reader for local files. BUrl::HasHost() is true for
    // file:/// URLs even though Host() is empty, so test the host string.
#if B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5
    BUrl mediaURL(url.utf8().legacyCStringPointer());
#else
    BUrl mediaURL(url.utf8().legacyCStringPointer(), false);
#endif
    entry_ref ref;
    bool hasLocalFile = false;
    // Fetched by WebCore already (startPrefetchHaiku()).
    if (m_prefetchedHaiku && !m_downloadPath.isEmpty() && !m_downloadFile) {
        auto* file = new BFile(m_downloadPath.utf8().legacyCStringPointer(), B_READ_ONLY);
        m_downloadFile = file;
        if (file->InitCheck() != B_OK)
            return;
    }
    if (mediaURL.Protocol() == "file" && (mediaURL.Host().IsEmpty() || mediaURL.Host() == "localhost")) {
        BString path = BUrl::UrlDecode(mediaURL.Path());
        hasLocalFile = get_ref_for_path(path.String(), &ref) == B_OK;
    }
#if USE(CURL)
    if (!hasLocalFile && (mediaURL.Protocol() == "http" || mediaURL.Protocol() == "https")) {
        MediaDownloadCancellation cancellation { m_abort, m_downloadLock, m_activeDownload };
        if (isHLSPlaylistURL(mediaURL)) {
            if (!downloadHLSByteRangeMedia(mediaURL, cancellation, m_downloadPath, m_secondaryDownloadPath))
                return;
        } else if (auto stream = mediaStreamingEnabled() ? StreamingMediaIO::open(mediaURL.UrlString().String(), m_abort, m_userAgent, m_referrer) : nullptr)
            m_downloadFile = stream.release();
        else if (m_abort || !downloadMediaFile(mediaURL, cancellation, m_downloadPath))
            return;
        if (!m_downloadFile) {
            auto* file = new BFile(m_downloadPath.utf8().legacyCStringPointer(), B_READ_ONLY);
            m_downloadFile = file;
            if (file->InitCheck() != B_OK)
                return;
        }
        if (!m_secondaryDownloadPath.isEmpty()) {
            m_secondaryDownloadFile = new BFile(m_secondaryDownloadPath.utf8().legacyCStringPointer(), B_READ_ONLY);
            if (m_secondaryDownloadFile->InitCheck() != B_OK)
                return;
        }
    }
#endif
    m_mediaFile = m_downloadFile ? new BMediaFile(m_downloadFile)
        : hasLocalFile ? new BMediaFile(&ref) : new BMediaFile(mediaURL);
    if (m_secondaryDownloadFile)
        m_secondaryMediaFile = new BMediaFile(m_secondaryDownloadFile);

    bool codecTrace = false;
    if (const char* trace = std::getenv("SUMMIT_MEDIA_CODEC_TRACE"); trace && trace[0] == '1' && !trace[1]) {
        codecTrace = true;
        std::fprintf(stderr, "Summit media source: protocol=%s host=%s\n", mediaURL.Protocol().String(), mediaURL.Host().String());
    }
    BMediaFile* mediaFiles[] = { m_mediaFile, m_secondaryMediaFile };
    for (auto* mediaFile : mediaFiles) {
        if (!mediaFile || mediaFile->InitCheck() != B_OK)
            continue;
        int trackCount = mediaFile->CountTracks();
        if (codecTrace)
            std::fprintf(stderr, "Summit media tracks: host=%s file=%s count=%d\n",
                mediaURL.Host().String(), mediaFile == m_mediaFile ? "primary" : "secondary", trackCount);
        for (int i = trackCount - 1; i >= 0; i--) {
            BMediaTrack* track = mediaFile->TrackAt(i);
            if (!track) {
                if (codecTrace)
                    std::fprintf(stderr, "Summit media unavailable track: host=%s index=%d\n",
                        mediaURL.Host().String(), i);
                continue;
            }

            media_format encodedFormat = { };
            if (track->EncodedFormat(&encodedFormat) != B_OK) {
                if (codecTrace)
                    std::fprintf(stderr, "Summit media unreadable track: host=%s index=%d\n",
                        mediaURL.Host().String(), i);
                mediaFile->ReleaseTrack(track);
                continue;
            }

            media_format format = { };
            if (encodedFormat.type == B_MEDIA_ENCODED_VIDEO) {
                format.type = B_MEDIA_RAW_VIDEO;
                format.u.raw_video.display.format = B_RGB32;
            } else if (encodedFormat.type == B_MEDIA_ENCODED_AUDIO)
                format.type = B_MEDIA_RAW_AUDIO;
            else {
                mediaFile->ReleaseTrack(track);
                continue;
            }
            auto decodeStatus = track->DecodedFormat(&format);
            std::unique_ptr<SubstituteVideoDecoderHaiku> fallbackVideo;
            if (decodeStatus != B_OK && encodedFormat.type == B_MEDIA_ENCODED_VIDEO && !m_videoTrack) {
                fallbackVideo = SubstituteVideoDecoderHaiku::createForTrackWithoutDecoder(*track, format);
                if (fallbackVideo) {
                    format = fallbackVideo->decodedFormat();
                    decodeStatus = B_OK;
                }
            }
            if (decodeStatus != B_OK) {
                if (codecTrace)
                    std::fprintf(stderr, "Summit media rejected track: host=%s index=%d type=%u status=%ld\n",
                        mediaURL.Host().String(), i, static_cast<unsigned>(encodedFormat.type), long(decodeStatus));
                mediaFile->ReleaseTrack(track);
                continue;
            }

            if (format.IsVideo()) {
                if (m_videoTrack) {
                    mediaFile->ReleaseTrack(track);
                    continue;
                }
                if (const char* trace = std::getenv("SUMMIT_MEDIA_CODEC_TRACE"); trace && trace[0] == '1' && !trace[1]) {
                    media_codec_info codec = { };
                    status_t codecStatus = track->GetCodecInfo(&codec);
                    std::fprintf(stderr, "Summit media video codec: %s (%s), index=%d status=%ld\n",
                        codecStatus == B_OK ? codec.pretty_name : "unknown",
                        codecStatus == B_OK ? codec.short_name : "unknown", i, long(codecStatus));
                }
                m_substituteVideo = fallbackVideo ? WTF::move(fallbackVideo) : SubstituteVideoDecoderHaiku::createIfNeeded(*track, format);
                if (m_substituteVideo)
                    format = m_substituteVideo->decodedFormat();
                m_videoTrack = track;
                m_videoTrackFile = mediaFile;
                m_videoTrackIndex = i;
                m_hasVideo = true;

                m_frameBuffer = new BBitmap(
                    BRect(0, 0, format.Width() - 1, format.Height() - 1),
                    B_RGB32);

                if (m_audioTrack)
                    break;
            }

            if (format.IsAudio()) {
                if (m_audioTrack) {
                    mediaFile->ReleaseTrack(track);
                    continue;
                }
                // Without an audio output, connecting a sound player crashes the
                // system mixer (it starts a missing output node); a format with
                // wildcards takes the same path. Play such media without sound.
                if (!canPlayAudio(format.u.raw_audio)) {
                    mediaFile->ReleaseTrack(track);
                    continue;
                }
                if (const char* trace = std::getenv("SUMMIT_MEDIA_CODEC_TRACE"); trace && trace[0] == '1' && !trace[1]) {
                    media_codec_info codec = { };
                    status_t codecStatus = track->GetCodecInfo(&codec);
                    std::fprintf(stderr, "Summit media audio codec: %s (%s), index=%d status=%ld\n",
                        codecStatus == B_OK ? codec.pretty_name : "unknown",
                        codecStatus == B_OK ? codec.short_name : "unknown", i, long(codecStatus));
                }
                m_audioTrack = track;
                m_audioFormat = format.u.raw_audio;

                if (m_videoTrack && m_audioTrack)
                    break;
            }

            if (!format.IsVideo() && !format.IsAudio())
                mediaFile->ReleaseTrack(track);

        }
        if (m_videoTrack && m_audioTrack)
            break;
    }

    if (m_videoTrack || m_audioTrack) {
        bigtime_t duration = 0;
        if (m_videoTrack)
            duration = std::max(duration, m_videoTrack->Duration());
        if (m_audioTrack)
            duration = std::max(duration, m_audioTrack->Duration());
        m_duration = duration / 1000000.f;

        if (m_videoTrack) {
            if (m_paused) {
                m_substituteVideo = nullptr;
                m_videoTrackFile->ReleaseTrack(m_videoTrack);
                m_videoTrack = nullptr;
                if (mediaPlaybackTraceEnabled())
                    std::fprintf(stderr, "Summit media playback: player=%p videoDecoder=park index=%d\n",
                        this, m_videoTrackIndex);
            } else {
                m_videoThread = spawn_thread(videoThread, "video decoder", B_NORMAL_PRIORITY, this);
                if (m_videoThread >= 0)
                    resume_thread(m_videoThread);
            }
        }
    }

    // The BFile remains open until after BMediaFile is destroyed. Removing its
    // directory entry also cleans up when the WebProcess is killed.
    if (!m_downloadPath.isEmpty()) {
        unlink(m_downloadPath.utf8().legacyCStringPointer());
        m_downloadPath = { };
    }
    if (!m_secondaryDownloadPath.isEmpty()) {
        unlink(m_secondaryDownloadPath.utf8().legacyCStringPointer());
        m_secondaryDownloadPath = { };
    }
}



// #pragma mark - static methods

// Asking the Media Kit for its file formats loads its add-ons into this
// process: 27 ms on the X399, on the main thread when a page first asks what
// it can play. prepareMediaEngineTypesHaiku() starts it on a thread of its
// own when the process starts; a page that asks sooner waits for that.
static std::once_flag mimeTypeCacheOnce;

static const HashSet<String>& mimeTypeCache()
{
    static NeverDestroyed<HashSet<String>> cache;
    std::call_once(mimeTypeCacheOnce, [] {
        int32 cookie = 0;
        media_file_format mfi;

        // Add the types the Haiku Media Kit add-ons advertise support for
        while (get_next_file_format(&cookie, &mfi) == B_OK)
            cache.get().add(String::fromUTF8(mfi.mime_type));

        // The finite byte-range HLS path is handled before Media Kit sees the
        // completed MP4 files, so its playlist MIME types are not advertised by
        // Media Kit's file-format add-ons.
        cache.get().add("application/vnd.apple.mpegurl"_s);
        cache.get().add("application/x-mpegurl"_s);
    });
    return cache;
}

void prepareMediaEngineTypesHaiku()
{
    std::thread([] {
        rename_thread(find_thread(nullptr), "media types");
        mimeTypeCache();
    }).detach();
}

void MediaPlayerPrivate::getSupportedTypes(HashSet<String>& types)
{
    types = mimeTypeCache();
}

MediaPlayer::SupportsType MediaPlayerPrivate::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.type.isEmpty())
        return MediaPlayer::SupportsType::IsNotSupported;
    // Media Source Extensions belong to MediaPlayerPrivateHaikuMSE; this
    // engine plays complete files and finite HLS renditions.
    if (parameters.platformType == PlatformMediaDecodingType::MediaSource)
        return MediaPlayer::SupportsType::IsNotSupported;

    auto containerType = parameters.type.containerType();
    if (equalLettersIgnoringASCIICase(containerType, "application/vnd.apple.mpegurl"_s)
        || equalLettersIgnoringASCIICase(containerType, "application/x-mpegurl"_s))
        return MediaPlayer::SupportsType::MayBeSupported;

    // spec says we should not return "probably" if the codecs string is empty
    if (mimeTypeCache().contains(containerType)) {
        return parameters.type.codecs().isEmpty() ? MediaPlayer::SupportsType::MayBeSupported : MediaPlayer::SupportsType::IsSupported;
    }

    return MediaPlayer::SupportsType::IsNotSupported;
}

ColorSpace MediaPlayerPrivate::colorSpace()
{
    return ColorSpace::SRGB();
}

}

#endif
