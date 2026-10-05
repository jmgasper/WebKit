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
#include "LibWebRTCH264DecoderHaiku.h"

#if USE(LIBWEBRTC) && PLATFORM(HAIKU)

#include "LibWebRTCMacros.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <wtf/NumberOfCores.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <libyuv.h>
#include <webrtc/api/video/i420_buffer.h>
#include <webrtc/api/video/video_frame.h>
#include <webrtc/api/video_codecs/builtin_video_decoder_factory.h>
#include <webrtc/api/video_codecs/builtin_video_encoder_factory.h>
#include <webrtc/api/video_codecs/h264_profile_level_id.h>
#include <webrtc/api/video_codecs/video_decoder.h>
#include <webrtc/modules/video_coding/codecs/h264/h264_encoder_haiku.h>
#include <webrtc/modules/video_coding/codecs/h264/include/h264.h>
#include <webrtc/modules/video_coding/include/video_error_codes.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

// H.264 for WebRTC, from libraries of the system loaded when first needed:
// decoding through FFmpeg (libavcodec, which Haiku's media add-on uses too),
// encoding through OpenH264 (libwebrtc's own encoder for it, see
// h264_encoder_haiku.cc). Without them, H.264 is not offered. For FFmpeg only
// functions are looked up, and only the leading members of AVPacket and
// AVFrame are used, which have kept their places for many major versions;
// contexts are configured through AVOptions.

namespace WebCore {

namespace {

struct AVPacketPrefix {
    void* buf;
    int64_t pts;
    int64_t dts;
    uint8_t* data;
    int size;
};

struct AVFramePrefix {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extendedData;
    int width;
    int height;
    int sampleCount;
    int format;
};

// AVPixelFormat values that have not moved since FFmpeg 1.
constexpr int pixelFormatYUV420P = 0;
constexpr int pixelFormatYUV422P = 4;
constexpr int pixelFormatYUV444P = 5;
constexpr int pixelFormatYUVJ420P = 12;
constexpr int pixelFormatYUVJ422P = 13;
constexpr int pixelFormatYUVJ444P = 14;

// AVERROR(EAGAIN): Haiku's errno values are negative already.
constexpr int averrorEAGAIN = EDOM > 0 ? -EAGAIN : EAGAIN;

struct FFmpeg {
    void* (*findDecoderByName)(const char*);
    void* (*allocContext)(const void*);
    int (*open)(void*, const void*, void**);
    void (*freeContext)(void**);
    void* (*allocPacket)();
    void (*freePacket)(void**);
    int (*sendPacket)(void*, const void*);
    int (*receiveFrame)(void*, void*);
    void* (*allocFrame)();
    void (*freeFrame)(void**);
    int (*setOptionInt)(void*, const char*, int64_t, int);

    static const FFmpeg* load()
    {
        static const FFmpeg* loaded = [] () -> const FFmpeg* {
            if (const char* value = getenv("SUMMIT_WEBRTC_H264"); value && !strcmp(value, "0"))
                return nullptr;
            void* avcodec = nullptr;
            for (const char* name : { "libavcodec.so.62", "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so.58" }) {
                if ((avcodec = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
                    break;
            }
            void* avutil = nullptr;
            for (const char* name : { "libavutil.so.60", "libavutil.so.59", "libavutil.so.58", "libavutil.so.57", "libavutil.so.56" }) {
                if ((avutil = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
                    break;
            }
            if (!avcodec || !avutil) {
                fprintf(stderr, "Summit: no libavcodec; WebRTC offers no H.264\n");
                return nullptr;
            }
            auto* ffmpeg = new FFmpeg;
            bool complete = true;
            auto find = [&](void* library, const char* name) {
                void* symbol = dlsym(library, name);
                complete = complete && symbol;
                return symbol;
            };
            ffmpeg->findDecoderByName = reinterpret_cast<void* (*)(const char*)>(find(avcodec, "avcodec_find_decoder_by_name"));
            ffmpeg->allocContext = reinterpret_cast<void* (*)(const void*)>(find(avcodec, "avcodec_alloc_context3"));
            ffmpeg->open = reinterpret_cast<int (*)(void*, const void*, void**)>(find(avcodec, "avcodec_open2"));
            ffmpeg->freeContext = reinterpret_cast<void (*)(void**)>(find(avcodec, "avcodec_free_context"));
            ffmpeg->allocPacket = reinterpret_cast<void* (*)()>(find(avcodec, "av_packet_alloc"));
            ffmpeg->freePacket = reinterpret_cast<void (*)(void**)>(find(avcodec, "av_packet_free"));
            ffmpeg->sendPacket = reinterpret_cast<int (*)(void*, const void*)>(find(avcodec, "avcodec_send_packet"));
            ffmpeg->receiveFrame = reinterpret_cast<int (*)(void*, void*)>(find(avcodec, "avcodec_receive_frame"));
            ffmpeg->allocFrame = reinterpret_cast<void* (*)()>(find(avutil, "av_frame_alloc"));
            ffmpeg->freeFrame = reinterpret_cast<void (*)(void**)>(find(avutil, "av_frame_free"));
            ffmpeg->setOptionInt = reinterpret_cast<int (*)(void*, const char*, int64_t, int)>(find(avutil, "av_opt_set_int"));
            if (!complete || !ffmpeg->findDecoderByName("h264")) {
                fprintf(stderr, "Summit: libavcodec has no usable H.264 decoder; WebRTC offers no H.264\n");
                delete ffmpeg;
                return nullptr;
            }
            return ffmpeg;
        }();
        return loaded;
    }
};

class H264DecoderHaiku final : public webrtc::VideoDecoder {
public:
    explicit H264DecoderHaiku(const FFmpeg& ffmpeg)
        : m_ffmpeg(ffmpeg)
    {
    }

    ~H264DecoderHaiku() { Release(); }

    bool Configure(const Settings& settings) final
    {
        Release();
        const void* codec = m_ffmpeg.findDecoderByName("h264");
        m_context = m_ffmpeg.allocContext(codec);
        if (!m_context)
            return false;
        // Slices in parallel, not frames: frame threads would hold pictures back.
        int threads = std::clamp(std::min(settings.number_of_cores(), WTF::numberOfProcessorCores()), 1, 4);
        m_ffmpeg.setOptionInt(m_context, "threads", threads, 0);
        m_ffmpeg.setOptionInt(m_context, "thread_type", 2 /* FF_THREAD_SLICE */, 0);
        if (m_ffmpeg.open(m_context, codec, nullptr) < 0) {
            Release();
            return false;
        }
        m_packet = m_ffmpeg.allocPacket();
        m_frame = m_ffmpeg.allocFrame();
        return m_packet && m_frame;
    }

    int32_t Decode(const webrtc::EncodedImage& image, int64_t) final
    {
        if (!m_context || !m_callback)
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        if (!image.data() || !image.size())
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        auto* packet = static_cast<AVPacketPrefix*>(m_packet);
        packet->data = const_cast<uint8_t*>(image.data());
        packet->size = static_cast<int>(image.size());
        packet->pts = image.RtpTimestamp();
        int status = m_ffmpeg.sendPacket(m_context, m_packet);
        packet->data = nullptr;
        packet->size = 0;
        if (status < 0 && status != averrorEAGAIN)
            return WEBRTC_VIDEO_CODEC_ERROR;

        while (m_ffmpeg.receiveFrame(m_context, m_frame) >= 0) {
            auto* frame = static_cast<AVFramePrefix*>(m_frame);
            auto buffer = toI420(*frame);
            if (!buffer)
                continue;
            auto output = webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_rtp_timestamp(image.RtpTimestamp())
                .set_color_space(image.ColorSpace())
                .build();
            m_callback->Decoded(output, std::nullopt, std::nullopt);
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) final
    {
        m_callback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() final
    {
        if (m_frame)
            m_ffmpeg.freeFrame(&m_frame);
        if (m_packet)
            m_ffmpeg.freePacket(&m_packet);
        if (m_context)
            m_ffmpeg.freeContext(&m_context);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const final
    {
        DecoderInfo info;
        info.implementation_name = "FFmpeg (Summit)";
        info.is_hardware_accelerated = false;
        return info;
    }

    const char* ImplementationName() const final { return "FFmpeg (Summit)"; }

private:
    webrtc::scoped_refptr<webrtc::I420Buffer> toI420(const AVFramePrefix& frame)
    {
        if (frame.width <= 0 || frame.height <= 0)
            return nullptr;
        auto buffer = webrtc::I420Buffer::Create(frame.width, frame.height);
        int status = -1;
        switch (frame.format) {
        case pixelFormatYUV420P:
        case pixelFormatYUVJ420P:
            status = libyuv::I420Copy(frame.data[0], frame.linesize[0], frame.data[1], frame.linesize[1], frame.data[2], frame.linesize[2],
                buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(), frame.width, frame.height);
            break;
        case pixelFormatYUV422P:
        case pixelFormatYUVJ422P:
            status = libyuv::I422ToI420(frame.data[0], frame.linesize[0], frame.data[1], frame.linesize[1], frame.data[2], frame.linesize[2],
                buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(), frame.width, frame.height);
            break;
        case pixelFormatYUV444P:
        case pixelFormatYUVJ444P:
            status = libyuv::I444ToI420(frame.data[0], frame.linesize[0], frame.data[1], frame.linesize[1], frame.data[2], frame.linesize[2],
                buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(), frame.width, frame.height);
            break;
        default:
            break;
        }
        return status ? nullptr : buffer;
    }

    const FFmpeg& m_ffmpeg;
    void* m_context { nullptr };
    void* m_packet { nullptr };
    void* m_frame { nullptr };
    webrtc::DecodedImageCallback* m_callback { nullptr };
};

// libwebrtc's own decoders (VP8, VP9) and H.264 through FFmpeg.
class VideoDecoderFactoryHaiku final : public webrtc::VideoDecoderFactory {
public:
    VideoDecoderFactoryHaiku()
        : m_builtin(webrtc::CreateBuiltinVideoDecoderFactory())
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final
    {
        auto formats = m_builtin->GetSupportedFormats();
        if (FFmpeg::load()) {
            for (auto profile : { webrtc::H264Profile::kProfileConstrainedBaseline, webrtc::H264Profile::kProfileBaseline, webrtc::H264Profile::kProfileMain, webrtc::H264Profile::kProfileHigh }) {
                formats.push_back(webrtc::CreateH264Format(profile, webrtc::H264Level::kLevel5_1, "1"));
                formats.push_back(webrtc::CreateH264Format(profile, webrtc::H264Level::kLevel5_1, "0"));
            }
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) final
    {
        if (equalIgnoringASCIICase(StringView::fromLatin1(format.name.c_str()), "H264"_s)) {
            if (auto* ffmpeg = FFmpeg::load())
                return std::make_unique<H264DecoderHaiku>(*ffmpeg);
            return nullptr;
        }
        return m_builtin->Create(environment, format);
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> m_builtin;
};

static bool h264Enabled()
{
    const char* value = getenv("SUMMIT_WEBRTC_H264");
    return !value || strcmp(value, "0");
}

class VideoEncoderFactoryHaiku final : public webrtc::VideoEncoderFactory {
public:
    VideoEncoderFactoryHaiku()
        : m_builtin(webrtc::CreateBuiltinVideoEncoderFactory())
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final
    {
        auto formats = m_builtin->GetSupportedFormats();
        if (h264Enabled() && webrtc::IsOpenH264EncoderAvailableHaiku()) {
            // OpenH264 encodes Constrained Baseline, which decoders of these
            // profiles all take (libwebrtc's list for it).
            for (auto profile : { webrtc::H264Profile::kProfileBaseline, webrtc::H264Profile::kProfileConstrainedBaseline, webrtc::H264Profile::kProfileMain }) {
                formats.push_back(webrtc::CreateH264Format(profile, webrtc::H264Level::kLevel3_1, "1"));
                formats.push_back(webrtc::CreateH264Format(profile, webrtc::H264Level::kLevel3_1, "0"));
            }
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) final
    {
        if (equalIgnoringASCIICase(StringView::fromLatin1(format.name.c_str()), "H264"_s))
            return h264Enabled() ? webrtc::CreateOpenH264EncoderHaiku(environment, format) : nullptr;
        return m_builtin->Create(environment, format);
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> m_builtin;
};

} // namespace

std::unique_ptr<webrtc::VideoDecoderFactory> createVideoDecoderFactoryHaiku()
{
    return std::make_unique<VideoDecoderFactoryHaiku>();
}

std::unique_ptr<webrtc::VideoEncoderFactory> createVideoEncoderFactoryHaiku()
{
    return std::make_unique<VideoEncoderFactoryHaiku>();
}

} // namespace WebCore

#endif // USE(LIBWEBRTC) && PLATFORM(HAIKU)
