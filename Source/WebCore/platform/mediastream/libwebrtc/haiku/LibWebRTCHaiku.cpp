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
#include "LibWebRTCHaiku.h"

#if USE(LIBWEBRTC) && PLATFORM(HAIKU)

#include "HaikuAudioData.h"
#include "LibWebRTCH264DecoderHaiku.h"
#include "LibWebRTCAudioFormat.h"
#include "LibWebRTCVideoFrameUtilities.h"
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <webrtc/api/video/i420_buffer.h>
#include <webrtc/api/video/video_frame.h>
#include <webrtc/api/video_codecs/builtin_video_decoder_factory.h>
#include <webrtc/common_video/include/video_frame_buffer.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

namespace {

// A libwebrtc picture a Haiku frame shows.
class WebRTCPixels final : public VideoFrameHaikuPixels {
public:
    static Ref<WebRTCPixels> create(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>&& buffer)
    {
        return adoptRef(*new WebRTCPixels(WTF::move(buffer)));
    }

private:
    explicit WebRTCPixels(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>&& buffer)
        : m_buffer(WTF::move(buffer))
    {
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_buffer;
};

} // namespace

Ref<VideoFrameHaiku> videoFrameFromWebRTCBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>&& buffer, MediaTime presentationTime, VideoFrame::Rotation rotation, PlatformVideoColorSpace&& colorSpace)
{
    IntSize size(buffer->width(), buffer->height());
    if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
        if (auto* nv12 = buffer->GetNV12()) {
            std::array<VideoFrameHaiku::Plane, 3> planes { {
                { const_cast<uint8_t*>(nv12->DataY()), size_t(nv12->StrideY()) },
                { const_cast<uint8_t*>(nv12->DataUV()), size_t(nv12->StrideUV()) },
                { },
            } };
            return VideoFrameHaiku::create(VideoFrameHaiku::Format::NV12, size, planes, WebRTCPixels::create(WTF::move(buffer)), presentationTime, false, rotation, WTF::move(colorSpace));
        }
    }

    // An I420 buffer answers itself.
    auto i420 = buffer->ToI420();
    if (!i420) {
        i420 = webrtc::I420Buffer::Create(std::max(size.width(), 1), std::max(size.height(), 1));
        webrtc::I420Buffer::SetBlack(static_cast<webrtc::I420Buffer*>(i420.get()));
    }
    std::array<VideoFrameHaiku::Plane, 3> planes { {
        { const_cast<uint8_t*>(i420->DataY()), size_t(i420->StrideY()) },
        { const_cast<uint8_t*>(i420->DataU()), size_t(i420->StrideU()) },
        { const_cast<uint8_t*>(i420->DataV()), size_t(i420->StrideV()) },
    } };
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::I420, { i420->width(), i420->height() }, planes,
        WebRTCPixels::create(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(i420)), presentationTime, false, rotation, WTF::move(colorSpace));
    return frame;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> webRTCBufferFromVideoFrame(VideoFrameHaiku& frame)
{
    RefPtr<VideoFrameHaiku> source = &frame;
    if (frame.format() != VideoFrameHaiku::Format::I420) {
        source = frame.convertedTo(VideoFrameHaiku::Format::I420);
        if (!source)
            return nullptr;
    }
    auto& y = source->plane(0);
    auto& u = source->plane(1);
    auto& v = source->plane(2);
    // The wrapper keeps our pixels until libwebrtc lets go of them.
    return webrtc::WrapI420Buffer(source->size().width(), source->size().height(), y.data, y.stride, u.data, u.stride, v.data, v.stride,
        [pixels = RefPtr { &source->pixels() }] { });
}

// Provider.

UniqueRef<WebRTCProvider> WebRTCProvider::create()
{
    return makeUniqueRef<LibWebRTCProviderHaiku>();
}

bool WebRTCProvider::webRTCAvailable()
{
    return true;
}

std::unique_ptr<webrtc::VideoEncoderFactory> LibWebRTCProviderHaiku::createEncoderFactory()
{
    return createVideoEncoderFactoryHaiku();
}

std::unique_ptr<webrtc::VideoDecoderFactory> LibWebRTCProviderHaiku::createDecoderFactory()
{
    return createVideoDecoderFactoryHaiku();
}

// Incoming audio: libwebrtc's 16-bit samples as float.

Ref<RealtimeIncomingAudioSource> RealtimeIncomingAudioSource::create(Ref<webrtc::AudioTrackInterface>&& audioTrack, String&& audioTrackId)
{
    auto source = RealtimeIncomingAudioSourceHaiku::create(WTF::move(audioTrack), WTF::move(audioTrackId));
    source->start();
    return source;
}

RealtimeIncomingAudioSourceHaiku::RealtimeIncomingAudioSourceHaiku(Ref<webrtc::AudioTrackInterface>&& audioTrack, String&& audioTrackId)
    : RealtimeIncomingAudioSource(WTF::move(audioTrack), WTF::move(audioTrackId))
{
}

RealtimeIncomingAudioSourceHaiku::~RealtimeIncomingAudioSourceHaiku()
{
    stop();
}

void RealtimeIncomingAudioSourceHaiku::OnData(const void* audioData, int bitsPerSample, int sampleRate, size_t numberOfChannels, size_t numberOfFrames)
{
    if (bitsPerSample != 16 || !numberOfChannels || !numberOfFrames || sampleRate <= 0)
        return;
    size_t count = numberOfChannels * numberOfFrames;
    m_samples.resize(count);
    auto* in = static_cast<const int16_t*>(audioData);
    for (size_t i = 0; i < count; ++i)
        m_samples[i] = in[i] * (1.0f / 32768.0f);

    if (m_baseTime.isInvalid())
        m_baseTime = MediaTime::createWithDouble(MonotonicTime::now().secondsSinceEpoch().value());
    auto mediaTime = m_baseTime + MediaTime(m_numberOfFrames * 1000000 / uint64_t(sampleRate), 1000000);
    m_numberOfFrames += numberOfFrames;

    HaikuAudioData data(m_samples.span(), numberOfChannels);
    audioSamplesAvailable(mediaTime, data, HaikuAudioStreamDescription(sampleRate, numberOfChannels), numberOfFrames);
}

// Incoming video.

Ref<RealtimeIncomingVideoSource> RealtimeIncomingVideoSource::create(Ref<webrtc::VideoTrackInterface>&& videoTrack, String&& trackId)
{
    auto source = RealtimeIncomingVideoSourceHaiku::create(WTF::move(videoTrack), WTF::move(trackId));
    source->start();
    return source;
}

RealtimeIncomingVideoSourceHaiku::RealtimeIncomingVideoSourceHaiku(Ref<webrtc::VideoTrackInterface>&& videoTrack, String&& trackId)
    : RealtimeIncomingVideoSource(WTF::move(videoTrack), WTF::move(trackId))
{
}

RealtimeIncomingVideoSourceHaiku::~RealtimeIncomingVideoSourceHaiku()
{
    stop();
}

void RealtimeIncomingVideoSourceHaiku::OnFrame(const webrtc::VideoFrame& webrtcFrame)
{
    if (!isProducingData())
        return;

    auto rotation = videoRotationFromLibWebRTCVideoFrame(webrtcFrame);
    unsigned width = webrtcFrame.width();
    unsigned height = webrtcFrame.height();
    if (rotation == VideoFrame::Rotation::Left || rotation == VideoFrame::Rotation::Right)
        std::swap(width, height);

    auto frame = videoFrameFromWebRTCBuffer(webrtcFrame.video_frame_buffer(), MediaTime(webrtcFrame.timestamp_us(), 1000000), rotation,
        colorSpaceFromLibWebRTCVideoFrame(webrtcFrame).value_or(PlatformVideoColorSpace { }));

    notifyNewFrame();
    setIntrinsicSize(IntSize(width, height));
    videoFrameAvailable(frame.get(), metadataFromVideoFrame(webrtcFrame));
}

// Outgoing audio: 10 ms chunks of 16-bit samples, at most two channels, at
// the rate they were captured (libwebrtc resamples).

WTF_MAKE_TZONE_ALLOCATED_IMPL(RealtimeOutgoingAudioSourceHaiku);

Ref<RealtimeOutgoingAudioSource> RealtimeOutgoingAudioSource::create(Ref<MediaStreamTrackPrivate>&& audioSource)
{
    return RealtimeOutgoingAudioSourceHaiku::create(WTF::move(audioSource));
}

RealtimeOutgoingAudioSourceHaiku::RealtimeOutgoingAudioSourceHaiku(Ref<MediaStreamTrackPrivate>&& audioSource)
    : RealtimeOutgoingAudioSource(WTF::move(audioSource))
{
}

void RealtimeOutgoingAudioSourceHaiku::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription& description, size_t numberOfFrames)
{
    if (!is<HaikuAudioData>(audioData))
        return;
    auto& data = downcast<HaikuAudioData>(audioData);
    unsigned inChannels = data.channelCount();
    double rate = description.sampleRate();
    if (!inChannels || rate <= 0)
        return;
    unsigned outChannels = std::min(inChannels, 2u);
    if (rate != m_sampleRate || outChannels != m_channelCount) {
        m_pending.clear();
        m_sampleRate = rate;
        m_channelCount = outChannels;
    }

    auto samples = data.samples();
    numberOfFrames = std::min(numberOfFrames, data.frameCount());
    bool silenced = isSilenced();
    size_t start = m_pending.size();
    m_pending.grow(start + numberOfFrames * outChannels);
    for (size_t frame = 0; frame < numberOfFrames; ++frame) {
        for (unsigned channel = 0; channel < outChannels; ++channel) {
            float value = silenced ? 0 : samples[frame * inChannels + channel];
            m_pending[start + frame * outChannels + channel] = static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        }
    }

    size_t chunkFrames = static_cast<size_t>(m_sampleRate / 100);
    size_t chunkSamples = chunkFrames * outChannels;
    size_t offset = 0;
    while (chunkFrames && m_pending.size() - offset >= chunkSamples) {
        m_chunk.resize(chunkSamples);
        memcpy(m_chunk.mutableSpan().data(), m_pending.span().subspan(offset).data(), chunkSamples * sizeof(int16_t));
        sendAudioFrames(asBytes(m_chunk.span()), LibWebRTCAudioFormat::sampleSize, m_sampleRate, outChannels, chunkFrames);
        offset += chunkSamples;
    }
    if (offset)
        m_pending.removeAt(0, offset);
}

// Outgoing video: I420 for the encoders.

Ref<RealtimeOutgoingVideoSource> RealtimeOutgoingVideoSource::create(Ref<MediaStreamTrackPrivate>&& videoSource)
{
    return RealtimeOutgoingVideoSourceHaiku::create(WTF::move(videoSource));
}

RealtimeOutgoingVideoSourceHaiku::RealtimeOutgoingVideoSourceHaiku(Ref<MediaStreamTrackPrivate>&& videoSource)
    : RealtimeOutgoingVideoSource(WTF::move(videoSource))
{
}

void RealtimeOutgoingVideoSourceHaiku::videoFrameAvailable(VideoFrame& videoFrame, VideoFrameTimeMetadata)
{
    switch (videoFrame.rotation()) {
    case VideoFrame::Rotation::None:
        m_currentRotation = webrtc::kVideoRotation_0;
        break;
    case VideoFrame::Rotation::UpsideDown:
        m_currentRotation = webrtc::kVideoRotation_180;
        break;
    case VideoFrame::Rotation::Right:
        m_currentRotation = webrtc::kVideoRotation_90;
        break;
    case VideoFrame::Rotation::Left:
        m_currentRotation = webrtc::kVideoRotation_270;
        break;
    }

    RefPtr<VideoFrameHaiku> frame = &downcast<VideoFrameHaiku>(videoFrame);
    auto scaling = videoFrameScaling();
    if (scaling != 1) {
        IntSize size = frame->size();
        size.scale(scaling);
        // Encoders want even sizes.
        size = { std::max(2, size.width() & ~1), std::max(2, size.height() & ~1) };
        if (size != frame->size())
            frame = frame->resizedTo(size);
        if (!frame)
            return;
    }

    auto buffer = webRTCBufferFromVideoFrame(*frame);
    if (!buffer)
        return;
    PlatformVideoColorSpace colorSpace = frame->format() == VideoFrameHaiku::Format::I420 ? frame->colorSpace()
        : PlatformVideoColorSpace { PlatformVideoColorPrimaries::Smpte170m, PlatformVideoTransferCharacteristics::Smpte170m, PlatformVideoMatrixCoefficients::Smpte170m, false };
    sendFrame(WTF::move(buffer), colorSpace);
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> RealtimeOutgoingVideoSourceHaiku::createBlackFrame(size_t width, size_t height)
{
    auto buffer = webrtc::I420Buffer::Create(width, height);
    webrtc::I420Buffer::SetBlack(buffer.get());
    return buffer;
}

} // namespace WebCore

#endif // USE(LIBWEBRTC) && PLATFORM(HAIKU)
