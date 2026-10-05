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
#include "MediaPlayerPrivateHaikuMSE.h"

#include "MediaDecoderSelectionHaiku.h"

#if ENABLE(MEDIA_SOURCE)

#include "AudioTrackPrivate.h"
#include "ColorSpace.h"
#include "ContentType.h"
#include "GraphicsContext.h"
#include "Logging.h"
#include "MediaDescription.h"
#include "MediaSourcePrivateClient.h"
#include "NativeImage.h"
#include "PlatformTimeRanges.h"
#include "SharedBuffer.h"
#include "VideoTrackPrivate.h"
#include <span>
#include <wtf/Condition.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/StringView.h>

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#include "CoordinatedPlatformLayerBufferPlanesHaiku.h"
#include "CoordinatedPlatformLayerBufferProxy.h"
#define HAIKU_MSE_COMPOSITED_VIDEO 1
#endif

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

#include <MediaDecoder.h>
#include <MediaDefs.h>
#include <MediaFormats.h>
#include <MediaRoster.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace WebCore {

// FFmpeg's codec numbers, which Haiku's Media Kit uses to describe encoded
// streams (B_MISC_FORMAT_FAMILY, file format 'ffmp'). The NVDEC add-on
// registers itself for H.264 under the same description.
static constexpr uint32 ffmpegCodecH264 = 27;
static constexpr uint32 ffmpegCodecAAC = 86018;

static bool mseTraceEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("SUMMIT_MSE_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}

#define MSE_TRACE(...) do { if (mseTraceEnabled()) { std::fprintf(stderr, "Summit MSE: [%.3f] ", system_time() / 1000000.0); std::fprintf(stderr, __VA_ARGS__); } } while (0)

static int64_t toMicroseconds(const MediaTime& time)
{
    if (!time.isValid())
        return 0;
    if (time.isPositiveInfinite())
        return INT64_MAX;
    if (time.isNegativeInfinite())
        return INT64_MIN;
    return time.toMicroseconds();
}

static MediaTime fromMicroseconds(int64_t microseconds)
{
    return MediaTime(microseconds, 1000000);
}

// #pragma mark - Samples

MediaSampleHaiku::MediaSampleHaiku(Ref<HaikuSampleData>&& data, Ref<HaikuTrackConfiguration>&& configuration, TrackID trackID,
    const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, SampleFlags flags)
    : m_data(WTF::move(data))
    , m_configuration(WTF::move(configuration))
    , m_trackID(trackID)
    , m_presentationTime(presentationTime)
    , m_decodeTime(decodeTime)
    , m_duration(duration)
    , m_flags(flags)
{
}

FloatSize MediaSampleHaiku::presentationSize() const
{
    auto& info = m_configuration->info;
    return FloatSize(info.width, info.height);
}

void MediaSampleHaiku::offsetTimestampsBy(const MediaTime& offset)
{
    m_presentationTime += offset;
    m_decodeTime += offset;
}

void MediaSampleHaiku::setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime)
{
    m_presentationTime = presentationTime;
    m_decodeTime = decodeTime;
}

Ref<MediaSample> MediaSampleHaiku::createNonDisplayingCopy() const
{
    return create(m_data.copyRef(), m_configuration.copyRef(), m_trackID, m_presentationTime, m_decodeTime, m_duration,
        SampleFlags(m_flags | IsNonDisplaying));
}

Ref<MediaSample> MediaSampleHaiku::createCopyWithAdjustedStartTime(const MediaTime& offset) const
{
    MediaTime clamped = std::max(MediaTime::zeroTime(), std::min(offset, m_duration));
    return create(m_data.copyRef(), m_configuration.copyRef(), m_trackID, m_presentationTime + clamped, m_decodeTime + clamped,
        m_duration - clamped, m_flags);
}

// #pragma mark - Clock

int64_t HaikuMSEClock::now() const
{
    Locker locker { m_lock };
    if (!m_running)
        return m_base;
    return m_base + (system_time() - m_startedAt);
}

void HaikuMSEClock::start()
{
    Locker locker { m_lock };
    if (m_running)
        return;
    m_startedAt = system_time();
    m_running = true;
}

void HaikuMSEClock::stop()
{
    Locker locker { m_lock };
    if (!m_running)
        return;
    m_base += system_time() - m_startedAt;
    m_running = false;
}

void HaikuMSEClock::set(int64_t mediaTime)
{
    Locker locker { m_lock };
    m_base = mediaTime;
    m_startedAt = system_time();
}

// #pragma mark - Tracks

namespace {

class MediaDescriptionHaiku final : public MediaDescription {
public:
    static Ref<MediaDescriptionHaiku> create(const SummitMP4::TrackInfo& info)
    {
        return adoptRef(*new MediaDescriptionHaiku(info));
    }

    bool isVideo() const final { return m_kind == SummitMP4::TrackInfo::Kind::Video; }
    bool isAudio() const final { return m_kind == SummitMP4::TrackInfo::Kind::Audio; }
    bool isText() const final { return false; }

private:
    explicit MediaDescriptionHaiku(const SummitMP4::TrackInfo& info)
        : MediaDescription(String::fromLatin1(info.codec.c_str()))
        , m_kind(info.kind)
    {
    }
    SummitMP4::TrackInfo::Kind m_kind;
};

class VideoTrackPrivateHaiku final : public VideoTrackPrivate {
public:
    static Ref<VideoTrackPrivateHaiku> create(TrackID trackID) { return adoptRef(*new VideoTrackPrivateHaiku(trackID)); }
    TrackID id() const final { return m_trackID; }

private:
    explicit VideoTrackPrivateHaiku(TrackID trackID)
        : m_trackID(trackID)
    {
    }
    TrackID m_trackID;
};

class AudioTrackPrivateHaiku final : public AudioTrackPrivate {
public:
    static Ref<AudioTrackPrivateHaiku> create(TrackID trackID) { return adoptRef(*new AudioTrackPrivateHaiku(trackID)); }
    TrackID id() const final { return m_trackID; }

private:
    explicit AudioTrackPrivateHaiku(TrackID trackID)
        : m_trackID(trackID)
    {
    }
    TrackID m_trackID;
};

// A Media Kit decoder fed from memory instead of from a BMediaTrack.
class HaikuChunkDecoder final : public BMediaDecoder {
public:
    using Provider = Function<status_t(const void**, size_t*, media_header*)>;
    explicit HaikuChunkDecoder(Provider&& provider)
        : m_provider(WTF::move(provider))
    {
    }

protected:
    status_t GetNextChunk(const void** data, size_t* size, media_header* header) final { return m_provider(data, size, header); }

private:
    Provider m_provider;
};

bool makeEncodedFormat(uint32 codec, bool video, media_format& format)
{
    media_format_description description;
    description.family = B_MISC_FORMAT_FAMILY;
    description.u.misc.file_format = 'ffmp';
    description.u.misc.codec = codec;

    format = media_format();
    format.type = video ? B_MEDIA_ENCODED_VIDEO : B_MEDIA_ENCODED_AUDIO;
    BMediaFormats formats;
    if (formats.InitCheck() != B_OK)
        return false;
    // Use the format the decoder add-ons registered. MakeFormatFor() with this
    // one description registers a separate encoding that no decoder claims,
    // and BMediaDecoder::SetTo() then fails with B_ENTRY_NOT_FOUND.
    if (formats.GetFormatFor(description, &format) == B_OK)
        return true;
    return formats.MakeFormatFor(&description, 1, &format) == B_OK;
}

bool sameDecoding(const SummitMP4::TrackInfo& a, const SummitMP4::TrackInfo& b)
{
    return a.kind == b.kind && a.codec == b.codec && a.timescale == b.timescale && a.width == b.width && a.height == b.height
        && a.channels == b.channels && a.sampleRate == b.sampleRate && a.codecConfig == b.codecConfig
        && a.objectTypeIndication == b.objectTypeIndication;
}

bool isH264(const std::string& codec) { return codec == "avc1" || codec == "avc3"; }

// Whether no other picture refers to this H.264 sample: each of its slices has
// nal_ref_idc 0 (an IDR slice never has). Such a picture before a seek target
// need not be decoded at all, as FFmpeg's AVDISCARD_NONREF does. |lengthSize|
// is the NAL length field's size from the avcC record.
bool isDisposableH264(std::span<const uint8_t> sample, unsigned lengthSize)
{
    if (lengthSize < 1 || lengthSize > 4)
        return false;
    bool sawSlice = false;
    size_t offset = 0;
    while (offset + lengthSize < sample.size()) {
        size_t length = 0;
        for (unsigned i = 0; i < lengthSize; ++i)
            length = (length << 8) | sample[offset + i];
        offset += lengthSize;
        if (!length || length > sample.size() - offset)
            return false;
        uint8_t header = sample[offset];
        unsigned type = header & 0x1f;
        if (type == 5)
            return false;
        if (type == 1) {
            if (header & 0x60)
                return false;
            sawSlice = true;
        }
        offset += length;
    }
    return sawSlice;
}

// SUMMIT_MSE_SKIP_DISPOSABLE=0 decodes every picture before a seek target.
bool skipDisposableBeforeTarget()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_MSE_SKIP_DISPOSABLE");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}
bool isAAC(const SummitMP4::TrackInfo& info) { return info.codec == "mp4a" && (info.objectTypeIndication == 0x40 || info.objectTypeIndication == 0x67); }

} // namespace

// #pragma mark - Sample queue shared by both renderers

class HaikuMSESampleQueue {
public:
    HaikuMSESampleQueue(size_t capacity)
        : m_capacity(capacity)
    {
    }

    void enqueue(Ref<MediaSampleHaiku>&& sample)
    {
        Locker locker { m_lock };
        m_endOfStream = false;
        m_samples.append(WTF::move(sample));
        m_condition.notifyAll();
    }

    bool isReadyForMore()
    {
        Locker locker { m_lock };
        return m_samples.size() < m_capacity;
    }

    void requestReadyNotification(ThreadSafeWeakPtr<SourceBufferPrivateHaiku>&& buffer, TrackID trackID)
    {
        Locker locker { m_lock };
        m_notify = WTF::move(buffer);
        m_notifyTrackID = trackID;
        m_wantsNotification = true;
    }

    // Returns the new generation.
    unsigned flush()
    {
        Locker locker { m_lock };
        m_samples.clear();
        m_endOfStream = false;
        ++m_generation;
        m_condition.notifyAll();
        return m_generation;
    }

    void setEndOfStream()
    {
        Locker locker { m_lock };
        m_endOfStream = true;
        m_condition.notifyAll();
    }

    void abort()
    {
        Locker locker { m_lock };
        m_aborted = true;
        m_condition.notifyAll();
    }

    bool aborted() const { return m_aborted; }
    unsigned generation() const { return m_generation; }

    // Waits for a sample; returns its configuration, or null on abort.
    RefPtr<const HaikuTrackConfiguration> waitForSample(unsigned& generation)
    {
        Locker locker { m_lock };
        while (!m_aborted && m_samples.isEmpty())
            m_condition.wait(m_lock);
        if (m_aborted)
            return nullptr;
        generation = m_generation;
        return &m_samples.first()->configuration();
    }

    enum class TakeResult { Sample, Stop };
    // Blocks until a sample for this configuration and generation is available.
    // Stop means the decoder should drain and end: the stream ended, the
    // configuration changed, the queue was flushed or the renderer is closing.
    TakeResult take(const HaikuTrackConfiguration* configuration, unsigned generation, RefPtr<MediaSampleHaiku>& sample)
    {
        ThreadSafeWeakPtr<SourceBufferPrivateHaiku> notify;
        TrackID notifyTrackID = 0;
        {
            Locker locker { m_lock };
            while (!m_aborted && m_generation == generation && m_samples.isEmpty() && !m_endOfStream)
                m_condition.wait(m_lock);
            if (m_aborted || m_generation != generation || m_samples.isEmpty())
                return TakeResult::Stop;
            if (&m_samples.first()->configuration() != configuration)
                return TakeResult::Stop;
            sample = m_samples.takeFirst();
            if (m_wantsNotification && m_samples.size() <= m_capacity / 2) {
                m_wantsNotification = false;
                notify = WTF::move(m_notify);
                notifyTrackID = m_notifyTrackID;
            }
        }
        if (RefPtr buffer = notify.get())
            buffer->rendererReadyForMoreSamples(notifyTrackID);
        return TakeResult::Sample;
    }

    // Lets a renderer wait without holding a sample, waking on flush or abort.
    void waitFor(Seconds timeout, unsigned generation)
    {
        Locker locker { m_lock };
        if (m_aborted || m_generation != generation)
            return;
        m_condition.waitFor(m_lock, timeout);
    }

    bool endOfStreamAndEmpty()
    {
        Locker locker { m_lock };
        return m_endOfStream && m_samples.isEmpty();
    }

    void dropSamplesFor(const HaikuTrackConfiguration* configuration)
    {
        Locker locker { m_lock };
        while (!m_samples.isEmpty() && &m_samples.first()->configuration() == configuration)
            m_samples.removeFirst();
    }

private:
    const size_t m_capacity;
    Lock m_lock;
    Condition m_condition;
    Deque<Ref<MediaSampleHaiku>> m_samples WTF_GUARDED_BY_LOCK(m_lock);
    std::atomic<unsigned> m_generation { 0 };
    std::atomic<bool> m_aborted { false };
    bool m_endOfStream WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_wantsNotification WTF_GUARDED_BY_LOCK(m_lock) { false };
    ThreadSafeWeakPtr<SourceBufferPrivateHaiku> m_notify WTF_GUARDED_BY_LOCK(m_lock);
    TrackID m_notifyTrackID WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

// #pragma mark - Video

class HaikuMSEVideoRenderer {
    WTF_MAKE_NONCOPYABLE(HaikuMSEVideoRenderer);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HaikuMSEVideoRenderer);
public:
    HaikuMSEVideoRenderer(MediaPlayerPrivateHaikuMSE& player, const HaikuMSEClock& clock)
        : m_weakPlayer(player)
        , m_clock(clock)
        , m_queue(90)
    {
        m_thread = Thread::create("Summit MSE video"_s, [this] { run(); }, ThreadType::Graphics, Thread::QOS::UserInteractive);
    }

    ~HaikuMSEVideoRenderer()
    {
        m_queue.abort();
        if (m_thread)
            m_thread->waitForCompletion();
    }

    void enqueue(Ref<MediaSampleHaiku>&& sample)
    {
        if (!m_enqueuedSinceFlush.exchange(true))
            MSE_TRACE("video first sample enqueued pts=%.3f sync=%d\n", sample->presentationTime().toDouble(), sample->isSync());
        m_queue.enqueue(WTF::move(sample));
    }
    bool isReadyForMore() { return m_queue.isReadyForMore(); }
    void requestReadyNotification(ThreadSafeWeakPtr<SourceBufferPrivateHaiku>&& buffer, TrackID trackID) { m_queue.requestReadyNotification(WTF::move(buffer), trackID); }
    void setEndOfStream() { m_queue.setEndOfStream(); }
    void flush()
    {
        m_hasFrameSinceFlush = false;
        m_enqueuedSinceFlush = false;
        m_queue.flush();
        MSE_TRACE("video flush generation=%u\n", m_queue.generation());
    }
    // The picture at the current position is on screen, so the clock can run.
    bool hasFrameSinceFlush() const { return m_hasFrameSinceFlush; }

#if USE(SKIA)
    sk_sp<SkImage> currentFrame()
    {
        Locker locker { m_frameLock };
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
        // A picture that went to the compositor as planes becomes an image
        // only when something paints it (a canvas, or the video before it is
        // composited).
        if (!m_frame && m_framePlanes) {
            const auto& layout = m_framePlanesLayout;
            auto pixels = SkData::MakeUninitialized(size_t(layout.width) * 4 * layout.height);
            AddOnVideoDecoderHaiku::convertToBGRA(static_cast<const uint8_t*>(m_framePlanes->data()), layout,
                static_cast<uint8_t*>(pixels->writable_data()), size_t(layout.width) * 4);
            auto info = SkImageInfo::Make(layout.width, layout.height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
            m_frame = SkImages::RasterFromData(info, WTF::move(pixels), size_t(layout.width) * 4);
        }
#endif
        return m_frame;
    }
#endif
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
    void setLayerBufferProxy(RefPtr<CoordinatedPlatformLayerBufferProxy>&& proxy)
    {
        Locker locker { m_frameLock };
        m_layerBufferProxy = WTF::move(proxy);
    }
#endif
    IntSize frameSize()
    {
        Locker locker { m_frameLock };
        return m_frameSize;
    }
    unsigned presentedFrames() const { return m_presentedFrames; }
    unsigned droppedFrames() const { return m_droppedFrames; }

private:
    void run()
    {
        while (!m_queue.aborted()) {
            unsigned generation = 0;
            RefPtr configuration = m_queue.waitForSample(generation);
            if (!configuration)
                return;
            if (generation != m_lastGeneration) {
                m_lastGeneration = generation;
                m_presentedSinceFlush = false;
                m_nonDisplaying.clear();
                m_decodedBeforeTarget = 0;
                m_skippedBeforeTarget = 0;
            }
            if (!decodeWith(*configuration, generation))
                m_queue.dropSamplesFor(configuration.get());
        }
    }

    // Decodes every sample that has this configuration. Returns false if no
    // decoder can be made for it.
    bool decodeWith(const HaikuTrackConfiguration& configuration, unsigned generation)
    {
        auto& info = configuration.info;
        if (!isH264(info.codec) || !info.width || !info.height) {
            MSE_TRACE("video codec %s %ux%u is not supported\n", info.codec.c_str(), info.width, info.height);
            return false;
        }
        media_format format;
        if (!makeEncodedFormat(ffmpegCodecH264, true, format))
            return false;
        format.u.encoded_video.output.display.line_width = info.width;
        format.u.encoded_video.output.display.line_count = info.height;
        format.u.encoded_video.output.field_rate = 30;
        format.u.encoded_video.output.pixel_width_aspect = 1;
        format.u.encoded_video.output.pixel_height_aspect = 1;

        // The avcC record's byte 4 gives the NAL length field's size.
        const unsigned nalLengthSize = info.codecConfig.size() > 4 ? (info.codecConfig[4] & 3) + 1 : 0;
        const std::span<const uint8_t> codecConfig { info.codecConfig.data(), info.codecConfig.size() };
        RefPtr<MediaSampleHaiku> current;
        // The samples a decoder took while it was set up, given again to the
        // next one when it refused the stream.
        Vector<RefPtr<MediaSampleHaiku>> taken;
        size_t replayed = 0;
        bool keepTaken = true;
        auto nextChunk = [&](const void** data, size_t* size, media_header* header) -> status_t {
            RefPtr<MediaSampleHaiku> next;
            if (replayed < taken.size())
                next = taken[replayed++];
            else {
                while (true) {
                    if (m_queue.take(&configuration, generation, next) == HaikuMSESampleQueue::TakeResult::Stop) {
                        // After a flush (or when closing) the pictures still in
                        // the decoder are not wanted: it stops at once. At the
                        // end of the stream it gives them out first. The
                        // Raspberry Pi's decoder, told the stream ended, waits
                        // for the firmware to say so too, which after a flush
                        // never came: the video stopped for good.
                        return m_queue.aborted() || m_queue.generation() != generation ? B_CANCELED : B_LAST_BUFFER_ERROR;
                    }
                    // A seek decodes from the keyframe before the target (5 s of
                    // pictures on YouTube, 0.75 s of decoding at 60 frames/s);
                    // the pictures nothing refers to can be left out.
                    if (next->isNonDisplaying() && skipDisposableBeforeTarget()
                        && isDisposableH264({ next->bytes(), next->size() }, nalLengthSize)) {
                        ++m_skippedBeforeTarget;
                        continue;
                    }
                    break;
                }
                if (next->isNonDisplaying()) {
                    m_nonDisplaying.append(toMicroseconds(next->presentationTime()));
                    ++m_decodedBeforeTarget;
                }
                if (keepTaken)
                    taken.append(next);
            }
            current = WTF::move(next);
            std::memset(header, 0, sizeof(*header));
            header->type = B_MEDIA_ENCODED_VIDEO;
            header->start_time = toMicroseconds(current->presentationTime());
            header->size_used = current->size();
            if (current->isSync())
                header->u.encoded_video.field_flags = B_MEDIA_KEY_FRAME;
            *data = current->bytes();
            *size = current->size();
            return B_OK;
        };
        bigtime_t setupStarted = system_time();

        // The Raspberry Pi's own decoder first; see MediaDecoderSelectionHaiku.h.
        std::unique_ptr<AddOnVideoDecoderHaiku> addOn;
        if (AddOnVideoDecoderHaiku::canDecode(format, codecConfig)) {
            addOn = AddOnVideoDecoderHaiku::create(format, codecConfig, [&](const void** data, size_t* size, media_header* header) {
                return nextChunk(data, size, header);
            });
            if (!addOn)
                replayed = 0;
        }
        if (addOn)
            taken.clear();
        else if (m_queue.aborted() || m_queue.generation() != generation)
            return true;
        keepTaken = false;

        HaikuChunkDecoder decoder([&](const void** data, size_t* size, media_header* header) {
            return nextChunk(data, size, header);
        });
        status_t status = B_OK;
        media_codec_info codecInfo;
        std::memset(&codecInfo, 0, sizeof(codecInfo));
        int width = 0;
        int height = 0;
        size_t rowBytes = 0;
        if (addOn) {
            codecInfo = addOn->codecInfo();
            width = addOn->width();
            height = addOn->height();
            rowBytes = size_t(width) * 4;
        } else {
            status = decoder.SetTo(&format, info.codecConfig.data(), info.codecConfig.size());
            if (status != B_OK) {
                // The add-on the media kit chose failed (the Raspberry Pi 4's image
                // has the ROCK 5's Rockchip decoder first); see MediaDecoderSelectionHaiku.h.
                status = setSoftwareDecoderHaiku(decoder, format, info.codecConfig.data(), info.codecConfig.size());
                std::fprintf(stderr, "Summit media: no decoder add-on took MSE video at first; libavcodec status %ld\n", long(status));
            }
            if (status == B_OK) {
                // NVDEC runs out of pictures on 16 reference frames; see MediaDecoderSelectionHaiku.h.
                media_codec_info chosen = { };
                decoder.GetDecoderInfo(&chosen);
                if (hardwareDecoderCannotDecodeHaiku(chosen, codecConfig)) {
                    status = setSoftwareDecoderHaiku(decoder, format, info.codecConfig.data(), info.codecConfig.size());
                    std::fprintf(stderr, "Summit media: MSE H.264 with %u reference frames goes to libavcodec instead of %s (status %ld)\n",
                        h264MaxReferenceFrames(codecConfig).value_or(0), chosen.pretty_name, long(status));
                }
            }
            media_format output;
            output.type = B_MEDIA_RAW_VIDEO;
            output.u.raw_video = media_raw_video_format::wildcard;
            output.u.raw_video.display.format = B_RGB32;
            output.u.raw_video.display.line_width = info.width;
            output.u.raw_video.display.line_count = info.height;
            if (status == B_OK)
                status = decoder.SetOutputFormat(&output);
            decoder.GetDecoderInfo(&codecInfo);
            if (status == B_OK && output.u.raw_video.display.format != B_RGB32)
                status = B_MEDIA_BAD_FORMAT;
            width = output.u.raw_video.display.line_width;
            height = output.u.raw_video.display.line_count;
            rowBytes = output.u.raw_video.display.bytes_per_row;
            if (!rowBytes)
                rowBytes = size_t(width) * 4;
        }
        MSE_TRACE("video decoder %s %ux%u status=%ld setup=%.1f ms\n", codecInfo.pretty_name, info.width, info.height,
            long(status), (system_time() - setupStarted) / 1000.0);
        // A flush while the decoder was set up (it reads samples to find the
        // picture size) is no reason to drop what the next generation queued.
        if (m_queue.aborted() || m_queue.generation() != generation)
            return true;
        if (status != B_OK || width <= 0 || height <= 0)
            return false;

#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
        // The Raspberry Pi's pictures go to the compositor as they come, in
        // planes; see CoordinatedPlatformLayerBufferPlanesHaiku.h.
        // SUMMIT_VIDEO_PLANES=0 converts them to RGB here instead.
        static const bool sendPlanes = [] {
            const char* value = std::getenv("SUMMIT_VIDEO_PLANES");
            return !value || std::strcmp(value, "0");
        }();
        const bool planes = addOn && sendPlanes;
#endif
        while (!m_queue.aborted() && m_queue.generation() == generation) {
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
            sk_sp<SkData> pixels = takeBuffer(planes ? addOn->planesSize() : rowBytes * height);
            void* target = pixels->writable_data();
#elif USE(SKIA)
            sk_sp<SkData> pixels = takeBuffer(rowBytes * height);
            void* target = pixels->writable_data();
#else
            m_scratch.resize(rowBytes * height);
            void* target = m_scratch.mutableSpan().data();
#endif
            media_header header;
            std::memset(&header, 0, sizeof(header));
            media_decode_info decodeInfo;
            // Pictures before a seek target are decoded only to reach it.
            // The NVDEC add-on skips their pixel conversion when told the
            // first time it will be asked to show.
            if (!m_nonDisplaying.isEmpty())
                decodeInfo.time_to_decode = -(*std::max_element(m_nonDisplaying.begin(), m_nonDisplaying.end()) + 1);
            int64 frames = 0;
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
            if (planes)
                status = addOn->decodePlanes(target, &frames, &header, &decodeInfo);
            else
#endif
            status = addOn ? addOn->decode(target, rowBytes, &frames, &header, &decodeInfo)
                : decoder.Decode(target, &frames, &header, &decodeInfo);
            if (status != B_OK)
                break;
            if (m_queue.generation() != generation)
                break;
            int64_t pts = header.start_time;
            if (takeNonDisplaying(pts))
                continue;
            if (!waitUntilDue(pts, generation))
                break;
            if (!m_presentedSinceFlush && (m_decodedBeforeTarget || m_skippedBeforeTarget))
                MSE_TRACE("seek target reached: %u pictures decoded before it, %u left out\n", m_decodedBeforeTarget, m_skippedBeforeTarget);
            int64_t lateness = m_clock.isRunning() ? m_clock.now() - pts : 0;
            if (m_presentedSinceFlush && lateness > 80000) {
                ++m_droppedFrames;
                continue;
            }
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
            if (planes) {
                presentPlanes(WTF::move(pixels), addOn->layout());
                continue;
            }
#endif
#if USE(SKIA)
            auto imageInfo = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
            present(SkImages::RasterFromData(imageInfo, WTF::move(pixels), rowBytes), IntSize(width, height));
#else
            present(IntSize(width, height));
#endif
        }
        return true;
    }

    bool takeNonDisplaying(int64_t pts)
    {
        for (size_t i = 0; i < m_nonDisplaying.size(); ++i) {
            if (m_nonDisplaying[i] == pts) {
                m_nonDisplaying.removeAt(i);
                return true;
            }
        }
        return false;
    }

    bool waitUntilDue(int64_t pts, unsigned generation)
    {
        while (!m_queue.aborted() && m_queue.generation() == generation) {
            // The first frame after a flush is the picture at the seek target;
            // show it at once, even while paused.
            if (!m_presentedSinceFlush)
                return true;
            if (!m_clock.isRunning()) {
                m_queue.waitFor(10_ms, generation);
                continue;
            }
            int64_t wait = pts - m_clock.now();
            if (wait <= 3000)
                return true;
            m_queue.waitFor(Seconds::fromMicroseconds(std::min<int64_t>(wait - 2000, 20000)), generation);
        }
        return false;
    }

#if USE(SKIA)
    sk_sp<SkData> takeBuffer(size_t size)
    {
        // A buffer is free again once no image refers to it.
        for (auto& buffer : m_buffers) {
            if (buffer->unique() && buffer->size() == size)
                return buffer;
        }
        m_buffers.removeAllMatching([](auto& buffer) { return buffer->unique(); });
        auto buffer = SkData::MakeUninitialized(size);
        if (m_buffers.size() < 8)
            m_buffers.append(buffer);
        return buffer;
    }

    void present(sk_sp<SkImage>&& image, IntSize size)
    {
        if (!image)
            return;
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
        RefPtr<CoordinatedPlatformLayerBufferProxy> proxy;
#endif
        {
            Locker locker { m_frameLock };
            m_frame = image;
            m_frameSize = size;
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
            m_framePlanes = nullptr;
            proxy = m_layerBufferProxy;
#endif
        }
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
        // Hand the frame to the compositor directly, so a busy page thread
        // does not hold it back and no tile has to be repainted for it.
        if (proxy && proxy->isValid()) {
            if (RefPtr nativeImage = NativeImage::create(WTF::move(image)))
                proxy->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(nativeImage.releaseNonNull(), nullptr));
        }
#endif
        didPresent();
    }

#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
    void presentPlanes(sk_sp<SkData>&& planes, const AddOnVideoDecoderHaiku::I420Layout& layout)
    {
        RefPtr<CoordinatedPlatformLayerBufferProxy> proxy;
        {
            Locker locker { m_frameLock };
            m_frame = nullptr;
            m_framePlanes = planes;
            m_framePlanesLayout = layout;
            m_frameSize = IntSize(layout.width, layout.height);
            proxy = m_layerBufferProxy;
        }
        if (proxy && proxy->isValid())
            proxy->setDisplayBuffer(makeUnique<CoordinatedPlatformLayerBufferPlanesHaiku>(WTF::move(planes), layout));
        didPresent();
    }
#endif
#else
    void present(IntSize size)
    {
        {
            Locker locker { m_frameLock };
            m_frameSize = size;
        }
        didPresent();
    }
#endif

    void didPresent()
    {
        m_presentedSinceFlush = true;
        m_hasFrameSinceFlush = true;
        ++m_presentedFrames;
        if (m_repaintQueued.exchange(true))
            return;
        callOnMainThread([weakPlayer = m_weakPlayer, this] {
            RefPtr player = weakPlayer.get();
            if (!player)
                return;
            m_repaintQueued = false;
            player->videoFrameAvailable();
        });
    }

    WeakPtr<MediaPlayerPrivateHaikuMSE> m_weakPlayer;
    const HaikuMSEClock& m_clock;
    HaikuMSESampleQueue m_queue;
    RefPtr<Thread> m_thread;
    unsigned m_lastGeneration { UINT_MAX };
    bool m_presentedSinceFlush { false };
    Vector<int64_t> m_nonDisplaying;
    unsigned m_decodedBeforeTarget { 0 };
    unsigned m_skippedBeforeTarget { 0 };
    std::atomic<bool> m_repaintQueued { false };
    std::atomic<bool> m_hasFrameSinceFlush { false };
    std::atomic<bool> m_enqueuedSinceFlush { false };
    std::atomic<unsigned> m_presentedFrames { 0 };
    std::atomic<unsigned> m_droppedFrames { 0 };
    Lock m_frameLock;
#if USE(SKIA)
    Vector<sk_sp<SkData>> m_buffers;
    sk_sp<SkImage> m_frame WTF_GUARDED_BY_LOCK(m_frameLock);
#endif
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
    // The newest picture when it came as planes; m_frame is made from it on demand.
    sk_sp<SkData> m_framePlanes WTF_GUARDED_BY_LOCK(m_frameLock);
    AddOnVideoDecoderHaiku::I420Layout m_framePlanesLayout WTF_GUARDED_BY_LOCK(m_frameLock);
#endif
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
    RefPtr<CoordinatedPlatformLayerBufferProxy> m_layerBufferProxy WTF_GUARDED_BY_LOCK(m_frameLock);
#endif
#if USE(SKIA)
#else
    Vector<uint8_t> m_scratch;
#endif
    IntSize m_frameSize WTF_GUARDED_BY_LOCK(m_frameLock);
};

// #pragma mark - Audio

class HaikuMSEAudioRenderer {
    WTF_MAKE_NONCOPYABLE(HaikuMSEAudioRenderer);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HaikuMSEAudioRenderer);
public:
    HaikuMSEAudioRenderer(MediaPlayerPrivateHaikuMSE& player, const HaikuMSEClock& clock)
        : m_weakPlayer(player)
        , m_clock(clock)
        , m_queue(120)
    {
        m_thread = Thread::create("Summit MSE audio decoder"_s, [this] { run(); }, ThreadType::Audio, Thread::QOS::UserInteractive);
    }

    ~HaikuMSEAudioRenderer()
    {
        m_queue.abort();
        {
            Locker locker { m_pcmLock };
            m_pcmCondition.notifyAll();
        }
        if (m_thread)
            m_thread->waitForCompletion();
        releaseOutput();
    }

    void enqueue(Ref<MediaSampleHaiku>&& sample) { m_queue.enqueue(WTF::move(sample)); }
    bool isReadyForMore() { return m_queue.isReadyForMore(); }
    void requestReadyNotification(ThreadSafeWeakPtr<SourceBufferPrivateHaiku>&& buffer, TrackID trackID) { m_queue.requestReadyNotification(WTF::move(buffer), trackID); }
    void setEndOfStream() { m_queue.setEndOfStream(); }
    void flush()
    {
        m_queue.flush();
        Locker locker { m_pcmLock };
        m_pcm.clear();
        m_pcmFrames = 0;
        m_pcmCondition.notifyAll();
    }

    // Main thread.
    void setPlaying(bool playing)
    {
        m_playing = playing;
        updateOutput();
    }
    void setVolume(float volume)
    {
        m_volume = volume;
        if (m_soundPlayer)
            m_soundPlayer->SetVolume(m_muted ? 0 : volume);
    }
    void setMuted(bool muted)
    {
        m_muted = muted;
        updateOutput();
    }
    // Main thread, from the player's timer: the output needs the decoded
    // format, which is only known after the first chunk.
    void updateOutput()
    {
        bool wanted = m_playing && !m_muted && m_formatKnown;
        if (!wanted) {
            if (m_soundPlayer && m_outputStarted) {
                m_soundPlayer->Stop(false);
                m_outputStarted = false;
            }
            if (m_muted)
                releaseOutput();
            return;
        }
        if (!m_soundPlayer) {
            media_raw_audio_format format;
            {
                Locker locker { m_pcmLock };
                format = m_format;
            }
            BMediaRoster* roster = BMediaRoster::Roster();
            media_node output;
            if (!roster || roster->GetAudioOutput(&output) != B_OK)
                return;
            roster->ReleaseNode(output);
            m_soundPlayer = new BSoundPlayer(&format, "Summit MSE audio", playCallback, nullptr, this);
            if (m_soundPlayer->InitCheck() != B_OK) {
                MSE_TRACE("audio output failed: %ld\n", long(m_soundPlayer->InitCheck()));
                delete m_soundPlayer;
                m_soundPlayer = nullptr;
                return;
            }
            m_latency = m_soundPlayer->Latency();
            m_soundPlayer->SetVolume(m_volume);
            m_soundPlayer->SetHasData(true);
            MSE_TRACE("audio output %.0f Hz %lu channels latency=%.1f ms\n", format.frame_rate, (unsigned long)format.channel_count, m_latency / 1000.0);
        }
        if (!m_outputStarted) {
            m_soundPlayer->Start();
            m_outputStarted = true;
        }
    }

private:
    struct PCMBlock {
        int64_t pts { 0 };
        Vector<uint8_t> bytes;
        size_t offset { 0 };
    };

    void releaseOutput()
    {
        if (!m_soundPlayer)
            return;
        auto* soundPlayer = std::exchange(m_soundPlayer, nullptr);
        m_outputStarted = false;
        soundPlayer->Stop(true);
        delete soundPlayer;
    }

    void run()
    {
        while (!m_queue.aborted()) {
            unsigned generation = 0;
            RefPtr configuration = m_queue.waitForSample(generation);
            if (!configuration)
                return;
            if (!decodeWith(*configuration, generation))
                m_queue.dropSamplesFor(configuration.get());
        }
    }

    bool decodeWith(const HaikuTrackConfiguration& configuration, unsigned generation)
    {
        auto& info = configuration.info;
        if (!isAAC(info) || !info.sampleRate || !info.channels) {
            MSE_TRACE("audio codec %s (0x%x) is not supported\n", info.codec.c_str(), info.objectTypeIndication);
            return false;
        }
        media_format format;
        if (!makeEncodedFormat(ffmpegCodecAAC, false, format))
            return false;
        format.u.encoded_audio.output.frame_rate = info.sampleRate;
        format.u.encoded_audio.output.channel_count = info.channels;
        format.u.encoded_audio.output.byte_order = B_MEDIA_HOST_ENDIAN;
        format.u.encoded_audio.frame_size = 1024;

        RefPtr<MediaSampleHaiku> current;
        HaikuChunkDecoder decoder([&](const void** data, size_t* size, media_header* header) -> status_t {
            RefPtr<MediaSampleHaiku> next;
            if (m_queue.take(&configuration, generation, next) == HaikuMSESampleQueue::TakeResult::Stop)
                return B_LAST_BUFFER_ERROR;
            current = WTF::move(next);
            std::memset(header, 0, sizeof(*header));
            header->type = B_MEDIA_ENCODED_AUDIO;
            header->start_time = toMicroseconds(current->presentationTime());
            header->size_used = current->size();
            *data = current->bytes();
            *size = current->size();
            return B_OK;
        });
        status_t status = decoder.SetTo(&format, info.codecConfig.data(), info.codecConfig.size());
        if (status != B_OK)
            status = setSoftwareDecoderHaiku(decoder, format, info.codecConfig.data(), info.codecConfig.size());
        media_format output;
        output.type = B_MEDIA_RAW_AUDIO;
        output.u.raw_audio = media_raw_audio_format::wildcard;
        if (status == B_OK)
            status = decoder.SetOutputFormat(&output);
        MSE_TRACE("audio decoder %u Hz %u channels status=%ld format=0x%lx buffer=%lu\n", info.sampleRate, info.channels, long(status),
            (unsigned long)output.u.raw_audio.format, (unsigned long)output.u.raw_audio.buffer_size);
        if (status != B_OK || !output.u.raw_audio.buffer_size || !output.u.raw_audio.channel_count)
            return false;

        size_t sampleSize = output.u.raw_audio.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
        size_t frameSize = sampleSize * output.u.raw_audio.channel_count;
        if (!frameSize)
            return false;
        {
            Locker locker { m_pcmLock };
            bool changed = m_formatKnown && (m_format.frame_rate != output.u.raw_audio.frame_rate
                || m_format.channel_count != output.u.raw_audio.channel_count || m_format.format != output.u.raw_audio.format);
            m_format = output.u.raw_audio;
            m_frameSize = frameSize;
            if (changed)
                m_formatChanged = true;
        }
        if (!m_formatKnown.exchange(true) || m_formatChanged) {
            // The player owns this renderer, so it is alive while the player is.
            callOnMainThread([weakPlayer = m_weakPlayer, this] {
                if (!weakPlayer.get())
                    return;
                if (m_formatChanged.exchange(false))
                    releaseOutput();
                updateOutput();
            });
        }

        while (!m_queue.aborted() && m_queue.generation() == generation) {
            PCMBlock block;
            block.bytes.grow(output.u.raw_audio.buffer_size);
            media_header header;
            std::memset(&header, 0, sizeof(header));
            int64 frames = 0;
            status = decoder.Decode(block.bytes.mutableSpan().data(), &frames, &header, nullptr);
            if (status != B_OK || frames <= 0)
                break;
            block.bytes.shrink(std::min<size_t>(block.bytes.size(), size_t(frames) * frameSize));
            block.pts = header.start_time;
            Locker locker { m_pcmLock };
            if (m_queue.generation() != generation)
                break;
            m_pcmFrames += frames;
            m_pcm.append(WTF::move(block));
            // Hold about half a second; drop what the clock has already passed.
            while (!m_queue.aborted() && m_queue.generation() == generation && m_pcmFrames > m_format.frame_rate / 2) {
                if (!m_pcm.isEmpty() && m_clock.isRunning() && blockEnd(m_pcm.first()) < m_clock.now() - 200000) {
                    m_pcmFrames -= (m_pcm.first().bytes.size() - m_pcm.first().offset) / m_frameSize;
                    m_pcm.removeFirst();
                    continue;
                }
                m_pcmCondition.waitFor(m_pcmLock, 20_ms);
            }
        }
        return true;
    }

    int64_t blockEnd(const PCMBlock& block) const WTF_REQUIRES_LOCK(m_pcmLock)
    {
        return block.pts + int64_t(block.bytes.size() / m_frameSize * 1000000.0 / m_format.frame_rate);
    }

    static void playCallback(void* cookie, void* buffer, size_t size, const media_raw_audio_format&)
    {
        static_cast<HaikuMSEAudioRenderer*>(cookie)->fill(static_cast<uint8_t*>(buffer), size);
    }

    void fill(uint8_t* buffer, size_t size)
    {
        size_t written = 0;
        if (m_clock.isRunning()) {
            Locker locker { m_pcmLock };
            if (m_frameSize && m_format.frame_rate > 0) {
                double microsecondsPerFrame = 1000000.0 / m_format.frame_rate;
                // Media time at which the start of this buffer will be heard.
                int64_t target = m_clock.now() + m_latency;
                while (written < size && !m_pcm.isEmpty()) {
                    auto& block = m_pcm.first();
                    int64_t blockTime = block.pts + int64_t(block.offset / m_frameSize * microsecondsPerFrame);
                    int64_t outputTime = target + int64_t(written / m_frameSize * microsecondsPerFrame);
                    int64_t drift = blockTime - outputTime;
                    if (drift < -60000) {
                        // Behind the clock: skip ahead.
                        size_t skip = std::min(block.bytes.size() - block.offset, size_t(-drift / microsecondsPerFrame) * m_frameSize);
                        skip = std::max(skip, m_frameSize);
                        block.offset = std::min(block.bytes.size(), block.offset + skip);
                        m_pcmFrames -= skip / m_frameSize;
                    } else if (drift > 60000) {
                        // Ahead of the clock: wait with silence.
                        size_t silence = std::min(size - written, size_t(drift / microsecondsPerFrame) * m_frameSize);
                        if (!silence)
                            break;
                        std::memset(buffer + written, 0, silence);
                        written += silence;
                        continue;
                    } else {
                        size_t count = std::min(size - written, block.bytes.size() - block.offset);
                        count -= count % m_frameSize;
                        if (!count)
                            break;
                        std::memcpy(buffer + written, block.bytes.span().data() + block.offset, count);
                        block.offset += count;
                        written += count;
                        m_pcmFrames -= count / m_frameSize;
                    }
                    if (block.offset >= block.bytes.size())
                        m_pcm.removeFirst();
                }
                m_pcmCondition.notifyAll();
            }
        }
        if (written < size)
            std::memset(buffer + written, 0, size - written);
    }

    WeakPtr<MediaPlayerPrivateHaikuMSE> m_weakPlayer;
    const HaikuMSEClock& m_clock;
    HaikuMSESampleQueue m_queue;
    RefPtr<Thread> m_thread;
    Lock m_pcmLock;
    Condition m_pcmCondition;
    Deque<PCMBlock> m_pcm WTF_GUARDED_BY_LOCK(m_pcmLock);
    int64_t m_pcmFrames WTF_GUARDED_BY_LOCK(m_pcmLock) { 0 };
    media_raw_audio_format m_format WTF_GUARDED_BY_LOCK(m_pcmLock) { };
    size_t m_frameSize { 0 };
    std::atomic<bool> m_formatKnown { false };
    std::atomic<bool> m_formatChanged { false };
    std::atomic<int64_t> m_latency { 0 };
    BSoundPlayer* m_soundPlayer { nullptr };
    bool m_outputStarted { false };
    bool m_playing { false };
    bool m_muted { false };
    float m_volume { 1 };
};

// #pragma mark - Engine registration

class MediaPlayerFactoryHaikuMSE final : public MediaPlayerFactory {
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::HaikuMSE; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final { return adoptRef(*new MediaPlayerPrivateHaikuMSE(player)); }
    void getSupportedTypes(HashSet<String>& types) const final { MediaPlayerPrivateHaikuMSE::getSupportedTypes(types); }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final { return MediaPlayerPrivateHaikuMSE::supportsType(parameters); }
};

void MediaPlayerPrivateHaikuMSE::registerMediaEngine(MediaEngineRegistrar registrar)
{
    if (const char* value = std::getenv("SUMMIT_DISABLE_MSE"); value && value[0] == '1' && !value[1])
        return;
    registrar(makeUnique<MediaPlayerFactoryHaikuMSE>());
}

static const HashSet<String>& mseTypes()
{
    static NeverDestroyed types = HashSet<String> { "video/mp4"_s, "audio/mp4"_s };
    return types;
}

void MediaPlayerPrivateHaikuMSE::getSupportedTypes(HashSet<String>& types)
{
    types = mseTypes();
}

static bool isSupportedCodec(StringView codec, bool audioOnly)
{
    // H.264 in 8-bit 4:2:0 is what the NVDEC add-on decodes; High 10, 4:2:2
    // and 4:4:4 profiles (0x6e, 0x7a, 0xf4) are refused.
    if (!audioOnly && (codec.startsWith("avc1"_s) || codec.startsWith("avc3"_s))) {
        if (codec.length() >= 7) {
            auto profile = codec.substring(5, 2);
            if (equalLettersIgnoringASCIICase(profile, "6e"_s) || equalLettersIgnoringASCIICase(profile, "7a"_s) || equalLettersIgnoringASCIICase(profile, "f4"_s))
                return false;
        }
        return true;
    }
    return codec == "mp4a.40.2"_s || codec == "mp4a.40.5"_s || codec == "mp4a.40.29"_s || codec == "mp4a.40"_s || codec == "mp4a.67"_s;
}

MediaPlayer::SupportsType MediaPlayerPrivateHaikuMSE::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.platformType != PlatformMediaDecodingType::MediaSource)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto container = parameters.type.containerType().convertToASCIILowercase();
    // Loading a MediaSource asks with no type at all (the URL is a blob:).
    if (container.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    if (!mseTypes().contains(container))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto codecs = parameters.type.codecs();
    if (codecs.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    bool audioOnly = container == "audio/mp4"_s;
    for (auto& codec : codecs) {
        if (!isSupportedCodec(codec, audioOnly))
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

// #pragma mark - Player

MediaPlayerPrivateHaikuMSE::MediaPlayerPrivateHaikuMSE(MediaPlayer& player)
    : m_player(player)
    , m_playbackTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateHaikuMSE::PlaybackTimer"_s, this, &MediaPlayerPrivateHaikuMSE::playbackTimerFired)
{
}

MediaPlayerPrivateHaikuMSE::~MediaPlayerPrivateHaikuMSE()
{
    m_playbackTimer.stop();
    m_videoRenderer = nullptr;
    m_audioRenderer = nullptr;
}

#if !RELEASE_LOG_DISABLED
uint64_t MediaPlayerPrivateHaikuMSE::mediaPlayerLogIdentifier()
{
    return m_player.get()->mediaPlayerLogIdentifier();
}

const Logger& MediaPlayerPrivateHaikuMSE::mediaPlayerLogger()
{
    return m_player.get()->mediaPlayerLogger();
}
#endif

HaikuMSEVideoRenderer& MediaPlayerPrivateHaikuMSE::videoRenderer()
{
    if (!m_videoRenderer)
        m_videoRenderer = makeUnique<HaikuMSEVideoRenderer>(*this, m_clock);
    return *m_videoRenderer;
}

HaikuMSEAudioRenderer& MediaPlayerPrivateHaikuMSE::audioRenderer()
{
    if (!m_audioRenderer) {
        m_audioRenderer = makeUnique<HaikuMSEAudioRenderer>(*this, m_clock);
        m_audioRenderer->setVolume(m_volume);
        m_audioRenderer->setMuted(m_muted);
        m_audioRenderer->setPlaying(m_clock.isRunning());
    }
    return *m_audioRenderer;
}

void MediaPlayerPrivateHaikuMSE::load(const String&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}

void MediaPlayerPrivateHaikuMSE::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    MSE_TRACE("load player=%p\n", this);
    if (RefPtr mediaSourcePrivate = dynamicDowncast<MediaSourcePrivateHaiku>(client.mediaSourcePrivate())) {
        mediaSourcePrivate->setPlayer(this);
        m_mediaSourcePrivate = WTF::move(mediaSourcePrivate);
        client.reOpen();
    } else
        m_mediaSourcePrivate = MediaSourcePrivateHaiku::create(*this, client);
    setNetworkState(MediaPlayer::NetworkState::Loading);
}

void MediaPlayerPrivateHaikuMSE::cancelLoad()
{
    m_playbackTimer.stop();
    m_clock.stop();
    if (m_audioRenderer)
        m_audioRenderer->setPlaying(false);
}

void MediaPlayerPrivateHaikuMSE::play()
{
    MSE_TRACE("play at %.3f\n", currentTime().toDouble());
    m_paused = false;
    updatePlaybackState();
}

void MediaPlayerPrivateHaikuMSE::pause()
{
    MSE_TRACE("pause at %.3f\n", currentTime().toDouble());
    m_paused = true;
    updatePlaybackState();
}

FloatSize MediaPlayerPrivateHaikuMSE::naturalSize() const
{
    return m_naturalSize;
}

bool MediaPlayerPrivateHaikuMSE::hasVideo() const
{
    return m_mediaSourcePrivate && m_mediaSourcePrivate->hasVideo();
}

bool MediaPlayerPrivateHaikuMSE::hasAudio() const
{
    return m_mediaSourcePrivate && m_mediaSourcePrivate->hasAudio();
}

MediaTime MediaPlayerPrivateHaikuMSE::currentTime() const
{
    if (m_pendingSeek)
        return m_pendingSeek->time;
    auto time = fromMicroseconds(m_clock.now());
    auto duration = this->duration();
    if (duration.isValid() && !duration.isIndefinite() && time > duration)
        return duration;
    return time;
}

bool MediaPlayerPrivateHaikuMSE::timeIsProgressing() const
{
    return m_clock.isRunning();
}

MediaTime MediaPlayerPrivateHaikuMSE::duration() const
{
    return m_mediaSourcePrivate ? m_mediaSourcePrivate->duration() : MediaTime::zeroTime();
}

Ref<MediaTimePromise> MediaPlayerPrivateHaikuMSE::seekToTarget(const SeekTarget& target)
{
    MSE_TRACE("seek to %.3f\n", target.time.toDouble());
    if (auto previous = std::exchange(m_seekPromise, std::nullopt))
        previous->reject(PlatformMediaError::Cancelled);
    m_pendingSeek = target;
    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    auto promise = m_seekPromise->promise();
    m_reachedEnd = false;
    updatePlaybackState();

    RefPtr mediaSource = m_mediaSourcePrivate;
    if (!mediaSource) {
        std::exchange(m_seekPromise, std::nullopt)->reject(PlatformMediaError::NotReady);
        return promise;
    }
    mediaSource->waitForTarget(target)->whenSettled(RunLoop::currentSingleton(), [weakThis = WeakPtr { *this }](auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (!result) {
            if (auto seekPromise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                seekPromise->reject(result.error());
            return;
        }
        auto seekTime = *result;
        RefPtr mediaSource = protectedThis->m_mediaSourcePrivate;
        if (!mediaSource)
            return;
        mediaSource->reenqueueMediaForTime(seekTime)->whenSettled(RunLoop::currentSingleton(), [weakThis, seekTime](auto&& result) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis || !result)
                return;
            MSE_TRACE("seek settled at %.3f\n", seekTime.toDouble());
            protectedThis->m_pendingSeek.reset();
            protectedThis->m_clock.set(toMicroseconds(seekTime));
            if (auto seekPromise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                seekPromise->resolve(seekTime);
            protectedThis->updatePlaybackState();
        });
    });
    return promise;
}

void MediaPlayerPrivateHaikuMSE::setVolume(float volume)
{
    m_volume = volume;
    if (m_audioRenderer)
        m_audioRenderer->setVolume(volume);
}

void MediaPlayerPrivateHaikuMSE::setMuted(bool muted)
{
    m_muted = muted;
    if (m_audioRenderer)
        m_audioRenderer->setMuted(muted);
}

MediaPlayer::ReadyState MediaPlayerPrivateHaikuMSE::readyState() const
{
    return m_mediaSourcePrivate ? m_mediaSourcePrivate->mediaPlayerReadyState() : MediaPlayer::ReadyState::HaveNothing;
}

void MediaPlayerPrivateHaikuMSE::readyStateFromMediaSourceChanged()
{
    MSE_TRACE("readyState %d\n", int(readyState()));
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
    updatePlaybackState();
}

void MediaPlayerPrivateHaikuMSE::characteristicsFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateHaikuMSE::seekableRangesFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->seekableTimeRangesChanged();
}

void MediaPlayerPrivateHaikuMSE::mediaSourceHasRetrievedAllData()
{
    setNetworkState(MediaPlayer::NetworkState::Loaded);
}

const PlatformTimeRanges& MediaPlayerPrivateHaikuMSE::buffered() const
{
    // HTMLMediaElement asks the MediaSource for buffered ranges.
    return PlatformTimeRanges::emptyRanges();
}

void MediaPlayerPrivateHaikuMSE::updateDuration(const MediaTime& duration)
{
    if (m_duration == duration)
        return;
    m_duration = duration;
    if (RefPtr player = m_player.get())
        player->durationChanged();
}

void MediaPlayerPrivateHaikuMSE::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

void MediaPlayerPrivateHaikuMSE::setNetworkState(MediaPlayer::NetworkState state)
{
    if (state == m_networkState)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateHaikuMSE::updatePlaybackState()
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    bool run = !m_paused && !m_pendingSeek && mediaSource;
    bool reachedEnd = false;
    if (run) {
        auto now = currentTime();
        auto duration = this->duration();
        if (duration.isValid() && !duration.isIndefinite() && !duration.isPositiveInfinite() && now >= duration) {
            run = false;
            reachedEnd = true;
        } else if (m_clock.isRunning())
            run = mediaSource->hasFutureTime(now, MediaTime::zeroTime());
        else {
            run = mediaSource->hasFutureTime(now, MediaTime(1, 10));
            // Preroll: start the clock once the picture at this position has
            // been decoded, or every frame decoded meanwhile arrives late and
            // is dropped. Give up waiting after a second in case no decoder
            // can be made for the video.
            if (run && hasVideo() && (!m_videoRenderer || !m_videoRenderer->hasFrameSinceFlush())) {
                if (!m_prerollStartedAt)
                    m_prerollStartedAt = MonotonicTime::now();
                if (MonotonicTime::now() - m_prerollStartedAt < 1_s)
                    run = false;
            }
        }
    }
    if (run || m_paused)
        m_prerollStartedAt = { };
    if (run != m_clock.isRunning()) {
        if (run)
            m_clock.start();
        else
            m_clock.stop();
        bool stalled = !run && !m_paused && !m_pendingSeek;
        MSE_TRACE("clock %s at %.3f%s\n", run ? "started" : "stopped", currentTime().toDouble(), stalled ? (reachedEnd ? " (end)" : " (stalled)") : "");
        if (stalled != m_stalled) {
            m_stalled = stalled;
            if (RefPtr player = m_player.get())
                player->timeChanged();
        }
    }
    if (m_audioRenderer)
        m_audioRenderer->setPlaying(run);
    if (reachedEnd && !m_reachedEnd) {
        m_reachedEnd = true;
        if (RefPtr player = m_player.get())
            player->timeChanged();
    } else if (!reachedEnd)
        m_reachedEnd = false;

    if (!m_paused && !m_playbackTimer.isActive())
        m_playbackTimer.startRepeating(50_ms);
    else if (m_paused)
        m_playbackTimer.stop();
}

void MediaPlayerPrivateHaikuMSE::playbackTimerFired()
{
    updatePlaybackState();
    if (mseTraceEnabled() && m_videoRenderer && ++m_statsTicks >= 100) {
        m_statsTicks = 0;
        MSE_TRACE("stats time=%.2f presented=%u dropped=%u running=%d\n", currentTime().toDouble(),
            m_videoRenderer->presentedFrames(), m_videoRenderer->droppedFrames(), m_clock.isRunning());
    }
    if (m_audioRenderer)
        m_audioRenderer->updateOutput();
}

void MediaPlayerPrivateHaikuMSE::videoFrameAvailable()
{
    if (!m_videoRenderer)
        return;
    RefPtr player = m_player.get();
    if (!player)
        return;
    auto size = m_videoRenderer->frameSize();
    if (!size.isEmpty() && FloatSize(size) != m_naturalSize) {
        m_naturalSize = FloatSize(size);
        player->sizeChanged();
    }
    if (!m_clock.isRunning() && !m_paused)
        updatePlaybackState();
    if (!m_announcedFirstFrame) {
        m_announcedFirstFrame = true;
        MSE_TRACE("first video frame %dx%d at %.3f\n", size.width(), size.height(), currentTime().toDouble());
        player->firstVideoFrameAvailable();
    }
#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
    if (m_layerBufferProxy && m_layerBufferProxy->isValid())
        return;
#endif
    player->repaint();
}

#if defined(HAIKU_MSE_COMPOSITED_VIDEO)
void MediaPlayerPrivateHaikuMSE::setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&& proxy)
{
    m_layerBufferProxy = proxy.copyRef();
    videoRenderer().setLayerBufferProxy(proxy.copyRef());
    MSE_TRACE("video goes to the compositor\n");
    if (auto image = videoRenderer().currentFrame()) {
        if (RefPtr nativeImage = NativeImage::create(WTF::move(image)))
            proxy->setInitialDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(nativeImage.releaseNonNull(), nullptr));
    }
}
#endif

void MediaPlayerPrivateHaikuMSE::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled() || !m_videoRenderer)
        return;
#if USE(SKIA)
    auto image = m_videoRenderer->currentFrame();
    if (!image)
        return;
    if (auto frame = NativeImage::create(WTF::move(image)))
        context.drawNativeImage(*frame, rect, FloatRect({ }, frame->size()));
#else
    UNUSED_PARAM(rect);
#endif
}

ColorSpace MediaPlayerPrivateHaikuMSE::colorSpace()
{
    return ColorSpace::SRGB();
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateHaikuMSE::videoPlaybackQualityMetrics()
{
    if (!m_mediaSourcePrivate)
        return std::nullopt;
    unsigned presented = m_videoRenderer ? m_videoRenderer->presentedFrames() : 0;
    unsigned dropped = m_videoRenderer ? m_videoRenderer->droppedFrames() : 0;
    return VideoPlaybackQualityMetrics {
        presented + dropped,
        dropped,
        0,
        0,
        0,
    };
}

// #pragma mark - MediaSourcePrivateHaiku

Ref<MediaSourcePrivateHaiku> MediaSourcePrivateHaiku::create(MediaPlayerPrivateHaikuMSE& player, MediaSourcePrivateClient& client)
{
    Ref source = adoptRef(*new MediaSourcePrivateHaiku(player, client));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}

MediaSourcePrivateHaiku::MediaSourcePrivateHaiku(MediaPlayerPrivateHaikuMSE& player, MediaSourcePrivateClient& client)
    : MediaSourcePrivate(client)
    , m_player(player)
{
}

MediaSourcePrivateHaiku::~MediaSourcePrivateHaiku() = default;

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateHaiku::player() const
{
    return m_player.get();
}

void MediaSourcePrivateHaiku::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = downcast<MediaPlayerPrivateHaikuMSE>(player);
}

MediaSourcePrivate::AddStatus MediaSourcePrivateHaiku::addSourceBuffer(const ContentType& contentType, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& outPrivate)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = contentType
    };
    if (MediaPlayerPrivateHaikuMSE::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported)
        return AddStatus::NotSupported;
    MSE_TRACE("addSourceBuffer %s\n", reinterpret_cast<const char*>(contentType.raw().utf8().data()));
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(SourceBufferPrivateHaiku::create(*this));
        outPrivate = m_sourceBuffers.last();
    }
    outPrivate->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

void MediaSourcePrivateHaiku::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    callOnMainThread([weakPlayer = m_player, duration] {
        if (RefPtr player = weakPlayer.get())
            player->updateDuration(duration);
    });
}

void MediaSourcePrivateHaiku::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->notifyActiveSourceBuffersChanged();
}

// #pragma mark - SourceBufferPrivateHaiku

Ref<SourceBufferPrivateHaiku> SourceBufferPrivateHaiku::create(MediaSourcePrivateHaiku& parent)
{
    return adoptRef(*new SourceBufferPrivateHaiku(parent));
}

SourceBufferPrivateHaiku::SourceBufferPrivateHaiku(MediaSourcePrivateHaiku& parent)
    : SourceBufferPrivate(parent)
#if !RELEASE_LOG_DISABLED
    , m_logger(parent.haikuPlayer()->mediaPlayerLogger())
    , m_logIdentifier(parent.haikuPlayer()->mediaPlayerLogIdentifier())
#endif
{
    m_parser.onInitSegment = [this](SummitMP4::InitSegment&& segment) { didParseInitializationSegment(WTF::move(segment)); };
    m_parser.onSample = [this](SummitMP4::Sample&& sample) { didParseSample(WTF::move(sample)); };
}

SourceBufferPrivateHaiku::~SourceBufferPrivateHaiku() = default;

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBufferPrivateHaiku::logChannel() const
{
    return LogMediaSource;
}
#endif

RefPtr<MediaPlayerPrivateHaikuMSE> SourceBufferPrivateHaiku::player() const
{
    RefPtr mediaSource = dynamicDowncast<MediaSourcePrivateHaiku>(m_mediaSource.get());
    return mediaSource ? mediaSource->haikuPlayer() : nullptr;
}

Ref<MediaPromise> SourceBufferPrivateHaiku::appendInternal(Ref<SharedBuffer>&& data)
{
    bool ok = !m_parseFailed;
    data->forEachSegment([&](std::span<const uint8_t> segment) {
        if (ok)
            ok = m_parser.append(segment.data(), segment.size());
    });
    if (!ok) {
        MSE_TRACE("append of %zu bytes could not be parsed\n", data->size());
        m_parseFailed = true;
        return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
    }
    return MediaPromise::createAndResolve();
}

void SourceBufferPrivateHaiku::resetParserStateInternal()
{
    m_parser.reset();
    m_parseFailed = false;
}

bool SourceBufferPrivateHaiku::canSwitchToType(const ContentType& contentType)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = contentType
    };
    return MediaPlayerPrivateHaikuMSE::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

void SourceBufferPrivateHaiku::didParseInitializationSegment(SummitMP4::InitSegment&& init)
{
    InitializationSegment segment;
    segment.duration = init.duration && init.movieTimescale ? MediaTime(int64_t(init.duration), init.movieTimescale) : MediaTime::invalidTime();
    auto previous = std::exchange(m_configurations, { });
    for (auto& track : init.tracks) {
        if (!track.timescale)
            continue;
        // Players append the same initialization segment again after seeks
        // and quality switches. Keeping its configuration keeps the decoder.
        RefPtr existing = previous.get(track.id);
        Ref configuration = existing && sameDecoding(existing->info, track) ? existing.releaseNonNull() : HaikuTrackConfiguration::create(track);
        m_configurations.set(track.id, configuration.copyRef());
        MSE_TRACE("init segment track=%u %s %ux%u %u Hz %u channels config=%zu\n", track.id, track.codec.c_str(),
            track.width, track.height, track.sampleRate, track.channels, track.codecConfig.size());
        if (track.kind == SummitMP4::TrackInfo::Kind::Video) {
            segment.videoTracks.append({ MediaDescriptionHaiku::create(track), VideoTrackPrivateHaiku::create(track.id) });
            if (!m_videoTrackID)
                m_videoTrackID = track.id;
        } else if (track.kind == SummitMP4::TrackInfo::Kind::Audio) {
            segment.audioTracks.append({ MediaDescriptionHaiku::create(track), AudioTrackPrivateHaiku::create(track.id) });
            if (!m_audioTrackID)
                m_audioTrackID = track.id;
        }
    }
    SourceBufferPrivate::didReceiveInitializationSegment(WTF::move(segment));
}

void SourceBufferPrivateHaiku::didParseSample(SummitMP4::Sample&& sample)
{
    RefPtr configuration = m_configurations.get(sample.trackID);
    if (!configuration)
        return;
    uint32_t timescale = configuration->info.timescale;
    auto flags = sample.isSync ? MediaSample::IsSync : MediaSample::None;
    SourceBufferPrivate::didReceiveSample(MediaSampleHaiku::create(HaikuSampleData::create(WTF::move(sample.data)), configuration.releaseNonNull(),
        sample.trackID, MediaTime(sample.presentationTime, timescale), MediaTime(sample.decodeTime, timescale),
        MediaTime(sample.duration, timescale), flags));
}

SourceBufferPrivateHaiku::Renderer SourceBufferPrivateHaiku::rendererForTrack(TrackID trackID) const
{
    if (m_videoTrackID && *m_videoTrackID == trackID)
        return Renderer::Video;
    if (m_audioTrackID && *m_audioTrackID == trackID)
        return Renderer::Audio;
    return Renderer::None;
}

void SourceBufferPrivateHaiku::flush(TrackID trackID)
{
    RefPtr player = this->player();
    if (!player)
        return;
    switch (rendererForTrack(trackID)) {
    case Renderer::Video:
        player->videoRenderer().flush();
        break;
    case Renderer::Audio:
        player->audioRenderer().flush();
        break;
    case Renderer::None:
        break;
    }
}

void SourceBufferPrivateHaiku::enqueueSample(Ref<MediaSample>&& sample, TrackID trackID)
{
    RefPtr player = this->player();
    if (!player)
        return;
    Ref haikuSample = static_cast<MediaSampleHaiku&>(sample.get());
    switch (rendererForTrack(trackID)) {
    case Renderer::Video:
        player->videoRenderer().enqueue(WTF::move(haikuSample));
        break;
    case Renderer::Audio:
        player->audioRenderer().enqueue(WTF::move(haikuSample));
        break;
    case Renderer::None:
        break;
    }
}

void SourceBufferPrivateHaiku::allSamplesInTrackEnqueued(TrackID trackID)
{
    RefPtr player = this->player();
    if (!player)
        return;
    switch (rendererForTrack(trackID)) {
    case Renderer::Video:
        player->videoRenderer().setEndOfStream();
        break;
    case Renderer::Audio:
        player->audioRenderer().setEndOfStream();
        break;
    case Renderer::None:
        break;
    }
}

bool SourceBufferPrivateHaiku::isReadyForMoreSamples(TrackID trackID)
{
    RefPtr player = this->player();
    if (!player)
        return false;
    switch (rendererForTrack(trackID)) {
    case Renderer::Video:
        return player->videoRenderer().isReadyForMore();
    case Renderer::Audio:
        return player->audioRenderer().isReadyForMore();
    case Renderer::None:
        break;
    }
    return true;
}

void SourceBufferPrivateHaiku::notifyClientWhenReadyForMoreSamples(TrackID trackID)
{
    RefPtr player = this->player();
    if (!player)
        return;
    switch (rendererForTrack(trackID)) {
    case Renderer::Video:
        player->videoRenderer().requestReadyNotification(ThreadSafeWeakPtr { *this }, trackID);
        break;
    case Renderer::Audio:
        player->audioRenderer().requestReadyNotification(ThreadSafeWeakPtr { *this }, trackID);
        break;
    case Renderer::None:
        break;
    }
}

void SourceBufferPrivateHaiku::rendererReadyForMoreSamples(TrackID trackID)
{
    ensureOnDispatcher([protectedThis = Ref { *this }, trackID] {
        protectedThis->provideMediaData(trackID);
    });
}

void SourceBufferPrivateHaiku::updateTrackIds(Vector<std::pair<TrackID, TrackID>>&& trackIdPairs)
{
    for (auto& [oldID, newID] : trackIdPairs) {
        if (m_videoTrackID == oldID)
            m_videoTrackID = newID;
        if (m_audioTrackID == oldID)
            m_audioTrackID = newID;
    }
    SourceBufferPrivate::updateTrackIds(WTF::move(trackIdPairs));
}

void SourceBufferPrivateHaiku::removedFromMediaSource()
{
    if (RefPtr player = this->player()) {
        if (m_videoTrackID)
            player->videoRenderer().flush();
        if (m_audioTrackID)
            player->audioRenderer().flush();
    }
    SourceBufferPrivate::removedFromMediaSource();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
