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
#include "MediaRecorderPrivateHaiku.h"

#if ENABLE(MEDIA_RECORDER) && PLATFORM(HAIKU) && USE(LIBWEBRTC)

#include "ContentType.h"
#include "HaikuAudioData.h"
#include "MediaStreamPrivate.h"
#include "SharedBuffer.h"
#include "VideoFrameHaiku.h"
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NumberOfCores.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <mkvmuxer/mkvmuxer.h>
#include <opus.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// Opus takes 48 kHz; recordings are stereo (a mono source is doubled).
static constexpr unsigned opusRate = 48000;
static constexpr unsigned opusChannels = 2;
static constexpr unsigned opusFrame = 960; // 20 ms
// Video frames waiting for the encoder beyond this are dropped.
static constexpr unsigned maximumQueuedVideoFrames = 4;

enum class VideoCodec : uint8_t { None, VP8, VP9 };

// libwebm writes into memory; fetchData() takes what is there.
class WebMWriter final : public mkvmuxer::IMkvWriter {
public:
    mkvmuxer::int32 Write(const void* buffer, mkvmuxer::uint32 length) final
    {
        m_data.append(std::span<const uint8_t>(static_cast<const uint8_t*>(buffer), length));
        m_position += length;
        return 0;
    }
    mkvmuxer::int64 Position() const final { return m_position; }
    mkvmuxer::int32 Position(mkvmuxer::int64) final { return -1; }
    bool Seekable() const final { return false; }
    void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) final { }

    Vector<uint8_t> take() { return std::exchange(m_data, { }); }

private:
    Vector<uint8_t> m_data;
    mkvmuxer::int64 m_position { 0 };
};

class HaikuWebMRecorder final : public ThreadSafeRefCounted<HaikuWebMRecorder, WTF::DestructionThread::Main> {
public:
    static Ref<HaikuWebMRecorder> create(VideoCodec codec, bool hasAudio, unsigned videoBitRate, unsigned audioBitRate, double videoFrameRate)
    {
        return adoptRef(*new HaikuWebMRecorder(codec, hasAudio, videoBitRate, audioBitRate, videoFrameRate));
    }

    ~HaikuWebMRecorder()
    {
        if (m_vpxReady)
            vpx_codec_destroy(&m_vpx);
        if (m_opus)
            opus_encoder_destroy(m_opus);
    }

    // Any thread.
    void appendVideo(Ref<VideoFrameHaiku>&& frame, bool black)
    {
        auto time = elapsed();
        if (!time)
            return;
        if (m_queuedVideoFrames >= maximumQueuedVideoFrames)
            return;
        ++m_queuedVideoFrames;
        m_queue->dispatch([protectedThis = Ref { *this }, frame = WTF::move(frame), black, time = *time]() mutable {
            --protectedThis->m_queuedVideoFrames;
            protectedThis->encodeVideo(WTF::move(frame), black, time);
        });
    }

    void appendAudio(std::span<const float> samples, unsigned channels, double rate, bool silent)
    {
        auto time = elapsed();
        if (!time || !channels || rate <= 0)
            return;
        Vector<float> copy(samples);
        m_queue->dispatch([protectedThis = Ref { *this }, samples = WTF::move(copy), channels, rate, silent, time = *time]() mutable {
            protectedThis->encodeAudio(WTF::move(samples), channels, rate, silent, time);
        });
    }

    // Main thread.
    void start()
    {
        Locker locker { m_timeLock };
        m_start = MonotonicTime::now();
    }

    void pause()
    {
        Locker locker { m_timeLock };
        if (!m_pausedAt)
            m_pausedAt = MonotonicTime::now();
    }

    void resume()
    {
        {
            Locker locker { m_timeLock };
            if (m_pausedAt) {
                m_pausedDuration += MonotonicTime::now() - *m_pausedAt;
                m_pausedAt = std::nullopt;
            }
        }
        m_queue->dispatch([protectedThis = Ref { *this }] {
            // The audio clock picks up from the next samples' arrival.
            protectedThis->m_audioAnchored = false;
        });
    }

    void stop(CompletionHandler<void()>&& completionHandler)
    {
        {
            Locker locker { m_timeLock };
            m_stopped = true;
        }
        m_queue->dispatch([protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
            protectedThis->finish();
            callOnMainThread(WTF::move(completionHandler));
        });
    }

    void fetch(CompletionHandler<void(Ref<FragmentedSharedBuffer>&&, double)>&& completionHandler)
    {
        m_queue->dispatch([protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
            auto data = protectedThis->m_writer.take();
            double timecode = protectedThis->m_lastFetchTime;
            protectedThis->m_lastFetchTime = protectedThis->m_latestTime.milliseconds();
            callOnMainThread([completionHandler = WTF::move(completionHandler), data = WTF::move(data), timecode]() mutable {
                completionHandler(SharedBuffer::create(WTF::move(data)), timecode);
            });
        });
    }

private:
    HaikuWebMRecorder(VideoCodec codec, bool hasAudio, unsigned videoBitRate, unsigned audioBitRate, double videoFrameRate)
        : m_queue(WorkQueue::create("Summit MediaRecorder"_s))
        , m_videoCodec(codec)
        , m_hasAudio(hasAudio)
        , m_videoBitRate(videoBitRate)
        , m_audioBitRate(audioBitRate)
        , m_videoFrameRate(videoFrameRate > 0 && videoFrameRate <= 240 ? videoFrameRate : 30)
    {
    }

    // Time since start, without pauses; nothing while paused or stopped.
    std::optional<Seconds> elapsed()
    {
        Locker locker { m_timeLock };
        if (!m_start || m_pausedAt || m_stopped)
            return std::nullopt;
        return MonotonicTime::now() - *m_start - m_pausedDuration;
    }

    // Work queue from here on.

    bool startSegment()
    {
        if (m_segmentStarted)
            return true;
        if (m_videoCodec != VideoCodec::None && m_videoSize.isEmpty())
            return false;
        if (!m_segment.Init(&m_writer))
            return false;
        m_segment.set_mode(mkvmuxer::Segment::kLive);
        m_segment.OutputCues(false);
        m_segment.GetSegmentInfo()->set_writing_app("Summit");
        m_segment.GetSegmentInfo()->set_muxing_app("Summit");
        if (m_videoCodec != VideoCodec::None) {
            m_videoTrack = m_segment.AddVideoTrack(m_videoSize.width(), m_videoSize.height(), 0);
            if (auto* track = static_cast<mkvmuxer::VideoTrack*>(m_segment.GetTrackByNumber(m_videoTrack))) {
                track->set_codec_id(m_videoCodec == VideoCodec::VP9 ? mkvmuxer::Tracks::kVp9CodecId : mkvmuxer::Tracks::kVp8CodecId);
                // The nominal rate, as Chrome writes it: a live file has no
                // duration or index to work it out from, and players that
                // count frames (Haiku's FFmpeg reader) otherwise take the
                // timestamps' unit, a thousand a second, as the rate.
                track->set_frame_rate(m_videoFrameRate);
            }
        }
        if (m_hasAudio && ensureOpus()) {
            m_audioTrack = m_segment.AddAudioTrack(opusRate, opusChannels, 0);
            if (auto* track = static_cast<mkvmuxer::AudioTrack*>(m_segment.GetTrackByNumber(m_audioTrack))) {
                track->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
                // OpusHead, RFC 7845.
                uint8_t head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, opusChannels,
                    uint8_t(m_opusPreSkip & 0xff), uint8_t(m_opusPreSkip >> 8),
                    uint8_t(opusRate & 0xff), uint8_t((opusRate >> 8) & 0xff), uint8_t((opusRate >> 16) & 0xff), uint8_t(opusRate >> 24),
                    0, 0, 0 };
                track->SetCodecPrivate(head, sizeof(head));
                track->set_codec_delay(uint64_t(m_opusPreSkip) * 1000000000ull / opusRate);
                track->set_seek_pre_roll(80000000);
            }
        }
        m_segmentStarted = true;
        return true;
    }

    bool ensureOpus()
    {
        if (m_opus)
            return true;
        int error = 0;
        m_opus = opus_encoder_create(opusRate, opusChannels, OPUS_APPLICATION_AUDIO, &error);
        if (!m_opus || error != OPUS_OK) {
            m_opus = nullptr;
            return false;
        }
        opus_encoder_ctl(m_opus, OPUS_SET_BITRATE(std::clamp<int>(m_audioBitRate, 6000, 510000)));
        opus_int32 lookahead = 0;
        opus_encoder_ctl(m_opus, OPUS_GET_LOOKAHEAD(&lookahead));
        m_opusPreSkip = lookahead;
        return true;
    }

    bool ensureVPX(IntSize size)
    {
        if (m_vpxReady)
            return true;
        vpx_codec_iface_t* interface = m_videoCodec == VideoCodec::VP9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
        vpx_codec_enc_cfg_t config;
        if (vpx_codec_enc_config_default(interface, &config, 0))
            return false;
        config.g_w = size.width();
        config.g_h = size.height();
        config.g_timebase.num = 1;
        config.g_timebase.den = 1000;
        config.rc_target_bitrate = std::max(m_videoBitRate / 1000, 100u);
        config.rc_end_usage = VPX_VBR;
        config.g_lag_in_frames = 0;
        config.g_error_resilient = 0;
        config.kf_mode = VPX_KF_AUTO;
        config.kf_max_dist = 300;
        config.g_threads = std::min(std::max(WTF::numberOfProcessorCores() / 2, 1), 8);
        if (vpx_codec_enc_init(&m_vpx, interface, &config, 0))
            return false;
        if (m_videoCodec == VideoCodec::VP9) {
            vpx_codec_control(&m_vpx, VP8E_SET_CPUUSED, 7);
            vpx_codec_control(&m_vpx, VP9E_SET_ROW_MT, 1);
            vpx_codec_control(&m_vpx, VP9E_SET_TILE_COLUMNS, size.width() >= 1280 ? 2 : 1);
        } else
            vpx_codec_control(&m_vpx, VP8E_SET_CPUUSED, -8);
        m_vpxReady = true;
        return true;
    }

    void encodeVideo(Ref<VideoFrameHaiku>&& frame, bool black, Seconds time)
    {
        if (m_finished)
            return;
        if (m_videoSize.isEmpty()) {
            m_videoSize = frame->presentationSize();
            if (frame->rotation() == VideoFrame::Rotation::Left || frame->rotation() == VideoFrame::Rotation::Right)
                m_videoSize = m_videoSize.transposedSize();
        }
        if (m_videoSize.isEmpty() || !ensureVPX(m_videoSize) || !startSegment())
            return;

        RefPtr<VideoFrameHaiku> picture = frame.ptr();
        if (picture->rotation() != VideoFrame::Rotation::None)
            picture = picture->uprightCopy();
        if (picture && picture->format() != VideoFrameHaiku::Format::I420)
            picture = picture->convertedTo(VideoFrameHaiku::Format::I420);
        if (picture && picture->size() != m_videoSize)
            picture = picture->resizedTo(m_videoSize);
        if (!picture)
            return;
        if (black) {
            auto blackPicture = VideoFrameHaiku::create(VideoFrameHaiku::Format::I420, m_videoSize, picture->presentationTime());
            for (unsigned plane = 0; plane < 3; ++plane) {
                auto& data = blackPicture->plane(plane);
                size_t rows = plane ? (m_videoSize.height() + 1) / 2 : m_videoSize.height();
                memset(data.data, plane ? 128 : 16, data.stride * rows);
            }
            picture = WTF::move(blackPicture);
        }

        vpx_image_t image;
        vpx_img_wrap(&image, VPX_IMG_FMT_I420, m_videoSize.width(), m_videoSize.height(), 1, picture->plane(0).data);
        for (unsigned plane = 0; plane < 3; ++plane) {
            image.planes[plane] = picture->plane(plane).data;
            image.stride[plane] = picture->plane(plane).stride;
        }

        int64_t pts = time.milliseconds();
        if (pts <= m_lastVideoPts)
            pts = m_lastVideoPts + 1;
        unsigned long duration = m_lastVideoPts >= 0 ? std::max<int64_t>(pts - m_lastVideoPts, 1) : 33;
        m_lastVideoPts = pts;
        if (vpx_codec_encode(&m_vpx, &image, pts, duration, 0, VPX_DL_REALTIME))
            return;
        collectVideoPackets();
    }

    void collectVideoPackets()
    {
        vpx_codec_iter_t iterator = nullptr;
        while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&m_vpx, &iterator)) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
                continue;
            uint64_t timestamp = uint64_t(std::max<int64_t>(packet->data.frame.pts, 0)) * 1000000ull;
            Packet output { m_videoTrack, timestamp, (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0,
                Vector<uint8_t>(std::span<const uint8_t>(static_cast<const uint8_t*>(packet->data.frame.buf), packet->data.frame.sz)) };
            m_lastVideoTimestamp = timestamp;
            m_videoPackets.append(WTF::move(output));
        }
        writePackets(false);
    }

    void encodeAudio(Vector<float>&& samples, unsigned channels, double rate, bool silent, Seconds time)
    {
        if (m_finished || !ensureOpus())
            return;
        if (!m_audioAnchored) {
            // Samples count from the first ones that came (after a pause, from
            // the first ones after it).
            m_audioOrigin = time - Seconds(double(m_audioSamplesEncoded + m_audioFifo.size() / opusChannels) / opusRate);
            m_audioAnchored = true;
        }

        // To 48 kHz stereo, linearly.
        size_t frames = samples.size() / channels;
        double step = rate / opusRate;
        auto sampleAt = [&](size_t frame, unsigned channel) -> float {
            if (silent)
                return 0;
            return samples[frame * channels + std::min(channel, channels - 1)];
        };
        if (rate == opusRate) {
            for (size_t frame = 0; frame < frames; ++frame) {
                for (unsigned channel = 0; channel < opusChannels; ++channel)
                    m_audioFifo.append(sampleAt(frame, channel));
            }
        } else {
            for (; m_resamplePosition < frames; m_resamplePosition += step) {
                size_t index = static_cast<size_t>(m_resamplePosition);
                double fraction = m_resamplePosition - index;
                for (unsigned channel = 0; channel < opusChannels; ++channel) {
                    float current = sampleAt(index, channel);
                    float next = index + 1 < frames ? sampleAt(index + 1, channel) : current;
                    m_audioFifo.append(current + (next - current) * fraction);
                }
            }
            m_resamplePosition -= frames;
        }

        if (!startSegment())
            return;
        encodeAudioFrames(false);
    }

    void encodeAudioFrames(bool flush)
    {
        size_t chunk = opusFrame * opusChannels;
        if (flush && m_audioFifo.size() % chunk)
            m_audioFifo.grow(m_audioFifo.size() + chunk - m_audioFifo.size() % chunk);
        uint8_t packet[4000];
        size_t offset = 0;
        while (m_audioFifo.size() - offset >= chunk) {
            int bytes = opus_encode_float(m_opus, m_audioFifo.span().subspan(offset).data(), opusFrame, packet, sizeof(packet));
            offset += chunk;
            if (bytes <= 0) {
                m_audioSamplesEncoded += opusFrame;
                continue;
            }
            auto timestamp = (m_audioOrigin + Seconds(double(m_audioSamplesEncoded) / opusRate)).nanoseconds();
            m_audioSamplesEncoded += opusFrame;
            uint64_t time = timestamp > 0 ? uint64_t(timestamp) : 0;
            m_lastAudioTimestamp = time;
            m_audioPackets.append({ m_audioTrack, time, true, Vector<uint8_t>(std::span<const uint8_t>(packet, bytes)) });
        }
        if (offset)
            m_audioFifo.removeAt(0, offset);
        writePackets(flush);
    }

    struct Packet {
        uint64_t track { 0 };
        uint64_t timestamp { 0 };
        bool isKey { false };
        Vector<uint8_t> data;
    };

    // Packets go into the file in time order across tracks.
    void writePackets(bool all)
    {
        if (!m_segmentStarted)
            return;
        bool hasVideo = m_videoCodec != VideoCodec::None;
        bool hasAudio = m_audioTrack;
        uint64_t limit = std::numeric_limits<uint64_t>::max();
        if (!all) {
            if (hasVideo)
                limit = std::min(limit, m_lastVideoTimestamp);
            if (hasAudio)
                limit = std::min(limit, m_lastAudioTimestamp);
        }
        while (true) {
            Deque<Packet>* source = nullptr;
            if (!m_videoPackets.isEmpty() && m_videoPackets.first().timestamp <= limit)
                source = &m_videoPackets;
            if (!m_audioPackets.isEmpty() && m_audioPackets.first().timestamp <= limit
                && (!source || m_audioPackets.first().timestamp < source->first().timestamp))
                source = &m_audioPackets;
            if (!source)
                break;
            auto packet = source->takeFirst();
            // A video track starts with a key frame.
            if (packet.track == m_videoTrack && !m_wroteVideoKey && !packet.isKey)
                continue;
            if (packet.track == m_videoTrack)
                m_wroteVideoKey = true;
            uint64_t timestamp = std::max(packet.timestamp, m_lastWrittenTimestamp);
            m_lastWrittenTimestamp = timestamp;
            m_segment.AddFrame(packet.data.span().data(), packet.data.size(), packet.track, timestamp, packet.isKey);
            m_latestTime = Seconds::fromNanoseconds(timestamp);
        }
    }

    void finish()
    {
        if (m_finished)
            return;
        m_finished = true;
        if (m_vpxReady) {
            vpx_codec_encode(&m_vpx, nullptr, -1, 1, 0, VPX_DL_REALTIME);
            collectVideoPackets();
        }
        if (m_opus && m_segmentStarted)
            encodeAudioFrames(true);
        if (!m_segmentStarted && m_videoCodec == VideoCodec::None && m_hasAudio)
            startSegment();
        writePackets(true);
        if (m_segmentStarted)
            m_segment.Finalize();
    }

    const Ref<WorkQueue> m_queue;
    const VideoCodec m_videoCodec;
    const bool m_hasAudio;
    const unsigned m_videoBitRate;
    const unsigned m_audioBitRate;
    const double m_videoFrameRate;
    std::atomic<unsigned> m_queuedVideoFrames { 0 };

    Lock m_timeLock;
    std::optional<MonotonicTime> m_start WTF_GUARDED_BY_LOCK(m_timeLock);
    std::optional<MonotonicTime> m_pausedAt WTF_GUARDED_BY_LOCK(m_timeLock);
    Seconds m_pausedDuration WTF_GUARDED_BY_LOCK(m_timeLock);
    bool m_stopped WTF_GUARDED_BY_LOCK(m_timeLock) { false };

    // Work queue.
    WebMWriter m_writer;
    mkvmuxer::Segment m_segment;
    bool m_segmentStarted { false };
    bool m_finished { false };
    uint64_t m_videoTrack { 0 };
    uint64_t m_audioTrack { 0 };
    IntSize m_videoSize;
    vpx_codec_ctx_t m_vpx { };
    bool m_vpxReady { false };
    int64_t m_lastVideoPts { -1 };
    OpusEncoder* m_opus { nullptr };
    unsigned m_opusPreSkip { 0 };
    Vector<float> m_audioFifo;
    double m_resamplePosition { 0 };
    bool m_audioAnchored { false };
    Seconds m_audioOrigin;
    uint64_t m_audioSamplesEncoded { 0 };
    Deque<Packet> m_videoPackets;
    Deque<Packet> m_audioPackets;
    uint64_t m_lastVideoTimestamp { 0 };
    uint64_t m_lastAudioTimestamp { 0 };
    uint64_t m_lastWrittenTimestamp { 0 };
    bool m_wroteVideoKey { false };
    Seconds m_latestTime;
    double m_lastFetchTime { 0 };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaRecorderPrivateHaiku);

struct RecorderConfiguration {
    VideoCodec videoCodec { VideoCodec::None };
    bool hasAudio { false };
    String mimeType;
};

static std::optional<RecorderConfiguration> configurationFor(const ContentType& contentType, bool hasVideo, bool hasAudio)
{
    auto container = contentType.containerType().convertToASCIILowercase();
    if (!container.isEmpty() && container != "video/webm"_s && container != "audio/webm"_s)
        return std::nullopt;
    if (container == "audio/webm"_s)
        hasVideo = false;

    RecorderConfiguration configuration;
    configuration.videoCodec = hasVideo ? VideoCodec::VP8 : VideoCodec::None;
    configuration.hasAudio = hasAudio;
    bool askedForAudio = false;
    for (auto& codec : contentType.codecs()) {
        auto name = codec.convertToASCIILowercase();
        if (name == "vp8"_s || name == "vp8.0"_s)
            configuration.videoCodec = hasVideo ? VideoCodec::VP8 : VideoCodec::None;
        else if (name == "vp9"_s || name.startsWith("vp09"_s) || name == "vp9.0"_s)
            configuration.videoCodec = hasVideo ? VideoCodec::VP9 : VideoCodec::None;
        else if (name == "opus"_s)
            askedForAudio = true;
        else
            return std::nullopt;
    }
    UNUSED_PARAM(askedForAudio);

    StringBuilder codecs;
    if (configuration.videoCodec == VideoCodec::VP8)
        codecs.append("vp8"_s);
    else if (configuration.videoCodec == VideoCodec::VP9)
        codecs.append("vp9"_s);
    if (configuration.hasAudio)
        codecs.append(codecs.isEmpty() ? ""_s : ","_s, "opus"_s);
    configuration.mimeType = makeString(configuration.videoCodec != VideoCodec::None ? "video/webm"_s : "audio/webm"_s, codecs.isEmpty() ? ""_s : "; codecs="_s, codecs.toString());
    return configuration;
}

bool MediaRecorderPrivateHaiku::isTypeSupported(const ContentType& contentType)
{
    return !!configurationFor(contentType, true, true);
}

std::unique_ptr<MediaRecorderPrivateHaiku> MediaRecorderPrivateHaiku::create(MediaStreamPrivate& stream, const MediaRecorderPrivateOptions& options)
{
    auto tracks = selectTracks(stream);
    auto configuration = configurationFor(ContentType(options.mimeType), !!tracks.videoTrack, !!tracks.audioTrack);
    if (!configuration || (configuration->videoCodec == VideoCodec::None && !configuration->hasAudio))
        return nullptr;
    auto bitRates = computeBitRates(options, &stream);
    double frameRate = 0;
    if (RefPtr videoTrack = tracks.videoTrack.get())
        frameRate = videoTrack->settings().frameRate();
    auto recorder = HaikuWebMRecorder::create(configuration->videoCodec, configuration->hasAudio, bitRates.video, bitRates.audio, frameRate);
    auto result = makeUnique<MediaRecorderPrivateHaiku>(stream, options, WTF::move(recorder));
    result->m_mimeType = configuration->mimeType;
    return result;
}

MediaRecorderPrivateHaiku::MediaRecorderPrivateHaiku(MediaStreamPrivate& stream, const MediaRecorderPrivateOptions&, Ref<HaikuWebMRecorder>&& recorder)
    : m_recorder(WTF::move(recorder))
{
    auto tracks = selectTracks(stream);
    if (RefPtr audioTrack = tracks.audioTrack.get())
        setAudioSource(&audioTrack->source());
    if (RefPtr videoTrack = tracks.videoTrack.get())
        setVideoSource(&videoTrack->source());
}

MediaRecorderPrivateHaiku::~MediaRecorderPrivateHaiku()
{
    setAudioSource(nullptr);
    setVideoSource(nullptr);
}

String MediaRecorderPrivateHaiku::mimeType() const
{
    return m_mimeType;
}

void MediaRecorderPrivateHaiku::startRecording(StartRecordingCallback&& callback)
{
    m_recorder->start();
    callback(String(m_mimeType), 0, 0);
}

void MediaRecorderPrivateHaiku::stopRecording(CompletionHandler<void()>&& completionHandler)
{
    setAudioSource(nullptr);
    setVideoSource(nullptr);
    m_recorder->stop(WTF::move(completionHandler));
}

void MediaRecorderPrivateHaiku::pauseRecording(CompletionHandler<void()>&& completionHandler)
{
    m_recorder->pause();
    completionHandler();
}

void MediaRecorderPrivateHaiku::resumeRecording(CompletionHandler<void()>&& completionHandler)
{
    m_recorder->resume();
    completionHandler();
}

void MediaRecorderPrivateHaiku::fetchData(FetchDataCallback&& completionHandler)
{
    m_recorder->fetch([completionHandler = WTF::move(completionHandler), mimeType = m_mimeType](Ref<FragmentedSharedBuffer>&& data, double timecode) mutable {
        completionHandler(WTF::move(data), mimeType, timecode);
    });
}

void MediaRecorderPrivateHaiku::videoFrameAvailable(VideoFrame& frame, VideoFrameTimeMetadata)
{
    m_recorder->appendVideo(downcast<VideoFrameHaiku>(frame), shouldMuteVideo());
}

void MediaRecorderPrivateHaiku::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& data, const AudioStreamDescription& description, size_t frameCount)
{
    if (!is<HaikuAudioData>(data))
        return;
    auto& audio = downcast<HaikuAudioData>(data);
    frameCount = std::min(frameCount, audio.frameCount());
    m_recorder->appendAudio(audio.samples().first(frameCount * audio.channelCount()), audio.channelCount(), description.sampleRate(), shouldMuteAudio());
}

} // namespace WebCore

#endif
