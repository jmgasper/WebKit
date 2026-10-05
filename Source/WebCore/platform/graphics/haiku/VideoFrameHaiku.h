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

#if ENABLE(VIDEO) && PLATFORM(HAIKU)

#include "IntSize.h"
#include "VideoFrame.h"
#include <array>
#include <wtf/Lock.h>

namespace WebCore {

class NativeImage;

// The pixels of a video picture in memory, shared by the frames made from
// them (clones, new timestamps). Defined in VideoFrameHaiku.cpp and, for a
// libwebrtc picture, LibWebRTCVideoFrameHaiku.cpp.
class VideoFrameHaikuPixels : public ThreadSafeRefCounted<VideoFrameHaikuPixels> {
public:
    virtual ~VideoFrameHaikuPixels() = default;
};

// The Haiku port's video frame: camera, screen and WebRTC pictures, frames a
// page makes (canvas capture, WebCodecs) and what the media players present.
// Pictures are I420 (cameras and decoders; what the encoders and the
// compositor's YUV shader take), BGRA (the screen, app_server's B_RGB32) or
// RGBA (page pixels). Planes are read-only once a frame is shared.
class VideoFrameHaiku final : public VideoFrame {
public:
    enum class Format : uint8_t { I420, NV12, BGRA, RGBA };

    struct Plane {
        uint8_t* data { nullptr };
        size_t stride { 0 };
    };

    // A frame with pixels of its own, tightly packed and not yet filled in:
    // write them through plane() before handing the frame to anyone.
    WEBCORE_EXPORT static Ref<VideoFrameHaiku> create(Format, IntSize, MediaTime presentationTime, bool isMirrored = false, Rotation = Rotation::None, PlatformVideoColorSpace&& = { });
    // A frame over pixels that live elsewhere; `pixels` keeps them alive.
    WEBCORE_EXPORT static Ref<VideoFrameHaiku> create(Format, IntSize, std::array<Plane, 3>, Ref<VideoFrameHaikuPixels>&&, MediaTime presentationTime, bool isMirrored = false, Rotation = Rotation::None, PlatformVideoColorSpace&& = { });

    Format format() const { return m_format; }
    IntSize size() const { return m_size; }
    unsigned planeCount() const;
    const Plane& plane(unsigned index) const LIFETIME_BOUND { return m_planes[index]; }
    VideoFrameHaikuPixels& pixels() const { return m_pixels.get(); }
    // RGB pictures without transparency (the screen, a camera).
    bool isOpaque() const { return m_isOpaque; }
    void setIsOpaque(bool opaque) { m_isOpaque = opaque; }

    // Conversions on the CPU (libyuv); null when not possible.
    WEBCORE_EXPORT RefPtr<VideoFrameHaiku> convertedTo(Format) const;
    WEBCORE_EXPORT RefPtr<VideoFrameHaiku> resizedTo(IntSize) const;
    // The picture turned as rotation() says, as a frame without rotation.
    WEBCORE_EXPORT RefPtr<VideoFrameHaiku> uprightCopy() const;
    // The picture as an image (BGRA, made once per frame).
    WEBCORE_EXPORT RefPtr<NativeImage> nativeImage() const;

    // VideoFrame
    IntSize presentationSize() const final { return m_size; }
    uint32_t pixelFormat() const final;

    // The FourCC pixelFormat() answers for each format.
    static uint32_t fourCC(Format);

private:
    VideoFrameHaiku(Format, IntSize, std::array<Plane, 3>, Ref<VideoFrameHaikuPixels>&&, MediaTime, bool isMirrored, Rotation, PlatformVideoColorSpace&&);

    Ref<VideoFrame> clone() final;

    const Format m_format;
    const IntSize m_size;
    const std::array<Plane, 3> m_planes;
    const Ref<VideoFrameHaikuPixels> m_pixels;
    bool m_isOpaque { false };
    mutable Lock m_imageLock;
    mutable RefPtr<NativeImage> m_image WTF_GUARDED_BY_LOCK(m_imageLock);
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::VideoFrameHaiku)
    static bool isType(const WebCore::VideoFrame&) { return true; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(VIDEO) && PLATFORM(HAIKU)
