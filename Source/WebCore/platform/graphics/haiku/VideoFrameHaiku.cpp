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
#include "VideoFrameHaiku.h"

#if ENABLE(VIDEO) && PLATFORM(HAIKU)

#include "NativeImage.h"
#include "PixelBuffer.h"
#include "PlatformVideoColorSpace.h"
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

#if USE(LIBWEBRTC)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <libyuv.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebCore {

namespace {

// Pixels a frame owns: one allocation for all planes, rows 32-byte aligned
// for libyuv's SIMD code.
class OwnedPixels final : public VideoFrameHaikuPixels {
public:
    static Ref<OwnedPixels> create(size_t size) { return adoptRef(*new OwnedPixels(size)); }
    ~OwnedPixels() { fastFree(m_data); }
    uint8_t* data() const { return m_data; }

private:
    explicit OwnedPixels(size_t size)
        : m_data(static_cast<uint8_t*>(fastAlignedMalloc(64, std::max<size_t>(size, 64))))
    {
    }
    uint8_t* m_data;
};

size_t alignedStride(size_t bytes)
{
    return (bytes + 31) & ~size_t(31);
}

bool isYUV(VideoFrameHaiku::Format format)
{
    return format == VideoFrameHaiku::Format::I420 || format == VideoFrameHaiku::Format::NV12;
}

#if USE(LIBWEBRTC)
// The YUV to RGB matrix for a frame's color space; streams that say nothing
// are BT.601 below 720 lines and BT.709 from there on, as decoders assume.
const libyuv::YuvConstants* yuvConstants(const PlatformVideoColorSpace& colorSpace, IntSize size, bool forRGBA)
{
    bool bt709 = size.height() >= 720;
    if (colorSpace.matrix) {
        switch (*colorSpace.matrix) {
        case PlatformVideoMatrixCoefficients::Bt709:
            bt709 = true;
            break;
        case PlatformVideoMatrixCoefficients::Bt470bg:
        case PlatformVideoMatrixCoefficients::Smpte170m:
            bt709 = false;
            break;
        default:
            break;
        }
    }
    bool fullRange = colorSpace.fullRange.value_or(false);
    // libyuv's "ABGR" functions take the U/V-swapped constants of the same matrix.
    if (forRGBA) {
        if (bt709)
            return fullRange ? &libyuv::kYvuF709Constants : &libyuv::kYvuH709Constants;
        return fullRange ? &libyuv::kYvuJPEGConstants : &libyuv::kYvuI601Constants;
    }
    if (bt709)
        return fullRange ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
    return fullRange ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
}
#endif

} // namespace

Ref<VideoFrameHaiku> VideoFrameHaiku::create(Format format, IntSize size, MediaTime presentationTime, bool isMirrored, Rotation rotation, PlatformVideoColorSpace&& colorSpace)
{
    size_t width = std::max(size.width(), 1);
    size_t height = std::max(size.height(), 1);
    std::array<Plane, 3> planes;
    RefPtr<OwnedPixels> pixels;
    switch (format) {
    case Format::I420: {
        size_t lumaStride = alignedStride(width);
        size_t chromaStride = alignedStride((width + 1) / 2);
        size_t chromaHeight = (height + 1) / 2;
        pixels = OwnedPixels::create(lumaStride * height + 2 * chromaStride * chromaHeight);
        planes[0] = { pixels->data(), lumaStride };
        planes[1] = { planes[0].data + lumaStride * height, chromaStride };
        planes[2] = { planes[1].data + chromaStride * chromaHeight, chromaStride };
        break;
    }
    case Format::NV12: {
        size_t lumaStride = alignedStride(width);
        size_t chromaStride = alignedStride(2 * ((width + 1) / 2));
        pixels = OwnedPixels::create(lumaStride * height + chromaStride * ((height + 1) / 2));
        planes[0] = { pixels->data(), lumaStride };
        planes[1] = { planes[0].data + lumaStride * height, chromaStride };
        break;
    }
    case Format::BGRA:
    case Format::RGBA: {
        size_t stride = alignedStride(4 * width);
        pixels = OwnedPixels::create(stride * height);
        planes[0] = { pixels->data(), stride };
        break;
    }
    }
    return create(format, size, planes, pixels.releaseNonNull(), presentationTime, isMirrored, rotation, WTF::move(colorSpace));
}

Ref<VideoFrameHaiku> VideoFrameHaiku::create(Format format, IntSize size, std::array<Plane, 3> planes, Ref<VideoFrameHaikuPixels>&& pixels, MediaTime presentationTime, bool isMirrored, Rotation rotation, PlatformVideoColorSpace&& colorSpace)
{
    return adoptRef(*new VideoFrameHaiku(format, size, planes, WTF::move(pixels), presentationTime, isMirrored, rotation, WTF::move(colorSpace)));
}

VideoFrameHaiku::VideoFrameHaiku(Format format, IntSize size, std::array<Plane, 3> planes, Ref<VideoFrameHaikuPixels>&& pixels, MediaTime presentationTime, bool isMirrored, Rotation rotation, PlatformVideoColorSpace&& colorSpace)
    : VideoFrame(presentationTime, isMirrored, rotation, WTF::move(colorSpace))
    , m_format(format)
    , m_size(size)
    , m_planes(planes)
    , m_pixels(WTF::move(pixels))
{
}

unsigned VideoFrameHaiku::planeCount() const
{
    switch (m_format) {
    case Format::I420:
        return 3;
    case Format::NV12:
        return 2;
    case Format::BGRA:
    case Format::RGBA:
        return 1;
    }
    return 1;
}

uint32_t VideoFrameHaiku::fourCC(Format format)
{
    switch (format) {
    case Format::I420:
        return 'I420';
    case Format::NV12:
        return 'NV12';
    case Format::BGRA:
        return 'BGRA';
    case Format::RGBA:
        return 'RGBA';
    }
    return 0;
}

uint32_t VideoFrameHaiku::pixelFormat() const
{
    if (m_isOpaque && m_format == Format::BGRA)
        return 'BGRX';
    if (m_isOpaque && m_format == Format::RGBA)
        return 'RGBX';
    return fourCC(m_format);
}

Ref<VideoFrame> VideoFrameHaiku::clone()
{
    auto frame = create(m_format, m_size, m_planes, m_pixels.copyRef(), presentationTime(), isMirrored(), rotation(), PlatformVideoColorSpace { colorSpace() });
    frame->m_isOpaque = m_isOpaque;
    return frame;
}

RefPtr<VideoFrameHaiku> VideoFrameHaiku::convertedTo(Format format) const
{
    if (format == m_format)
        return create(m_format, m_size, m_planes, m_pixels.copyRef(), presentationTime(), isMirrored(), rotation(), PlatformVideoColorSpace { colorSpace() });

#if USE(LIBWEBRTC)
    int width = m_size.width();
    int height = m_size.height();
    if (width <= 0 || height <= 0)
        return nullptr;

    PlatformVideoColorSpace targetColorSpace = colorSpace();
    if (isYUV(format) && !isYUV(m_format)) {
        // libyuv's RGB to YUV functions write limited-range BT.601.
        targetColorSpace = { PlatformVideoColorPrimaries::Smpte170m, PlatformVideoTransferCharacteristics::Smpte170m, PlatformVideoMatrixCoefficients::Smpte170m, false };
    } else if (!isYUV(format))
        targetColorSpace = { };

    auto result = create(format, m_size, presentationTime(), isMirrored(), rotation(), WTF::move(targetColorSpace));
    auto& to = result->m_planes;
    auto& from = m_planes;
    int status = -1;

    switch (m_format) {
    case Format::I420:
        switch (format) {
        case Format::BGRA:
            status = libyuv::I420ToARGBMatrix(from[0].data, from[0].stride, from[1].data, from[1].stride, from[2].data, from[2].stride, to[0].data, to[0].stride, yuvConstants(colorSpace(), m_size, false), width, height);
            break;
        case Format::RGBA:
            // libyuv's ABGR is R, G, B, A in memory; the swapped-plane call keeps the matrix.
            status = libyuv::I420ToARGBMatrix(from[0].data, from[0].stride, from[2].data, from[2].stride, from[1].data, from[1].stride, to[0].data, to[0].stride, yuvConstants(colorSpace(), m_size, true), width, height);
            break;
        case Format::NV12:
            status = libyuv::I420ToNV12(from[0].data, from[0].stride, from[1].data, from[1].stride, from[2].data, from[2].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, width, height);
            break;
        case Format::I420:
            break;
        }
        break;
    case Format::NV12:
        switch (format) {
        case Format::BGRA:
            status = libyuv::NV12ToARGBMatrix(from[0].data, from[0].stride, from[1].data, from[1].stride, to[0].data, to[0].stride, yuvConstants(colorSpace(), m_size, false), width, height);
            break;
        case Format::RGBA:
            // As NV21 with the U/V-swapped matrix: R, G, B, A in memory.
            status = libyuv::NV21ToARGBMatrix(from[0].data, from[0].stride, from[1].data, from[1].stride, to[0].data, to[0].stride, yuvConstants(colorSpace(), m_size, true), width, height);
            break;
        case Format::I420:
            status = libyuv::NV12ToI420(from[0].data, from[0].stride, from[1].data, from[1].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, to[2].data, to[2].stride, width, height);
            break;
        case Format::NV12:
            break;
        }
        break;
    case Format::BGRA:
        switch (format) {
        case Format::I420:
            status = libyuv::ARGBToI420(from[0].data, from[0].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, to[2].data, to[2].stride, width, height);
            break;
        case Format::NV12:
            status = libyuv::ARGBToNV12(from[0].data, from[0].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, width, height);
            break;
        case Format::RGBA:
            status = libyuv::ARGBToABGR(from[0].data, from[0].stride, to[0].data, to[0].stride, width, height);
            break;
        case Format::BGRA:
            break;
        }
        break;
    case Format::RGBA:
        switch (format) {
        case Format::I420:
            status = libyuv::ABGRToI420(from[0].data, from[0].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, to[2].data, to[2].stride, width, height);
            break;
        case Format::NV12:
            status = libyuv::ABGRToNV12(from[0].data, from[0].stride, to[0].data, to[0].stride, to[1].data, to[1].stride, width, height);
            break;
        case Format::BGRA:
            status = libyuv::ABGRToARGB(from[0].data, from[0].stride, to[0].data, to[0].stride, width, height);
            break;
        case Format::RGBA:
            break;
        }
        break;
    }
    if (status)
        return nullptr;
    result->m_isOpaque = m_isOpaque || isYUV(m_format);
    return result;
#else
    UNUSED_PARAM(format);
    return nullptr;
#endif
}

RefPtr<VideoFrameHaiku> VideoFrameHaiku::resizedTo(IntSize size) const
{
    if (size == m_size)
        return create(m_format, m_size, m_planes, m_pixels.copyRef(), presentationTime(), isMirrored(), rotation(), PlatformVideoColorSpace { colorSpace() });
    if (size.isEmpty() || m_size.isEmpty())
        return nullptr;

#if USE(LIBWEBRTC)
    // NV12 is scaled as I420.
    RefPtr<const VideoFrameHaiku> source = this;
    RefPtr<VideoFrameHaiku> converted;
    if (m_format == Format::NV12) {
        converted = convertedTo(Format::I420);
        if (!converted)
            return nullptr;
        source = converted;
    }

    auto result = create(source->m_format, size, presentationTime(), isMirrored(), rotation(), PlatformVideoColorSpace { colorSpace() });
    auto& to = result->m_planes;
    auto& from = source->m_planes;
    int status = -1;
    if (source->m_format == Format::I420) {
        status = libyuv::I420Scale(from[0].data, from[0].stride, from[1].data, from[1].stride, from[2].data, from[2].stride, m_size.width(), m_size.height(),
            to[0].data, to[0].stride, to[1].data, to[1].stride, to[2].data, to[2].stride, size.width(), size.height(), libyuv::kFilterBox);
    } else {
        status = libyuv::ARGBScale(from[0].data, from[0].stride, m_size.width(), m_size.height(),
            to[0].data, to[0].stride, size.width(), size.height(), libyuv::kFilterBox);
    }
    if (status)
        return nullptr;
    result->m_isOpaque = m_isOpaque;
    return result;
#else
    UNUSED_PARAM(size);
    return nullptr;
#endif
}

RefPtr<VideoFrameHaiku> VideoFrameHaiku::uprightCopy() const
{
#if USE(LIBWEBRTC)
    RefPtr<const VideoFrameHaiku> source = this;
    RefPtr<VideoFrameHaiku> converted;
    if (m_format == Format::NV12) {
        converted = convertedTo(Format::I420);
        if (!converted)
            return nullptr;
        source = converted;
    }
    libyuv::RotationMode mode = libyuv::kRotate0;
    IntSize size = m_size;
    switch (rotation()) {
    case Rotation::None:
        break;
    case Rotation::UpsideDown:
        mode = libyuv::kRotate180;
        break;
    case Rotation::Right:
        mode = libyuv::kRotate90;
        size = size.transposedSize();
        break;
    case Rotation::Left:
        mode = libyuv::kRotate270;
        size = size.transposedSize();
        break;
    }
    auto result = create(source->m_format, size, presentationTime(), isMirrored(), Rotation::None, PlatformVideoColorSpace { colorSpace() });
    auto& to = result->m_planes;
    auto& from = source->m_planes;
    int status;
    if (source->m_format == Format::I420) {
        status = libyuv::I420Rotate(from[0].data, from[0].stride, from[1].data, from[1].stride, from[2].data, from[2].stride,
            to[0].data, to[0].stride, to[1].data, to[1].stride, to[2].data, to[2].stride, m_size.width(), m_size.height(), mode);
    } else
        status = libyuv::ARGBRotate(from[0].data, from[0].stride, to[0].data, to[0].stride, m_size.width(), m_size.height(), mode);
    if (status)
        return nullptr;
    result->m_isOpaque = m_isOpaque;
    return result;
#else
    return nullptr;
#endif
}

RefPtr<NativeImage> VideoFrameHaiku::nativeImage() const
{
#if USE(SKIA)
    {
        Locker locker { m_imageLock };
        if (m_image)
            return m_image;
    }

    RefPtr<const VideoFrameHaiku> rgb = this;
    if (isYUV(m_format)) {
        rgb = convertedTo(Format::BGRA);
        if (!rgb)
            return nullptr;
    }

    auto colorType = rgb->m_format == Format::RGBA ? kRGBA_8888_SkColorType : kBGRA_8888_SkColorType;
    auto alphaType = rgb->m_isOpaque || rgb != this ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType;
    auto info = SkImageInfo::Make(m_size.width(), m_size.height(), colorType, alphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, rgb->m_planes[0].data, rgb->m_planes[0].stride);
    // The image keeps the pixels alive.
    auto* pixels = &rgb->m_pixels.get();
    pixels->ref();
    auto image = SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        static_cast<VideoFrameHaikuPixels*>(context)->deref();
    }, pixels);
    if (!image)
        return nullptr;

    RefPtr nativeImage = NativeImage::create(WTF::move(image));
    Locker locker { m_imageLock };
    m_image = nativeImage;
    return nativeImage;
#else
    return nullptr;
#endif
}

// VideoFrame's platform functions.

static void copyRows(const uint8_t* source, size_t sourceStride, uint8_t* destination, size_t destinationStride, size_t rowBytes, size_t rows)
{
    for (size_t row = 0; row < rows; ++row) {
        memcpy(destination, source, rowBytes);
        source += sourceStride;
        destination += destinationStride;
    }
}

// Copies plane `index` of a frame from a page's buffer laid out as `layout`.
static bool copyPlaneFrom(std::span<const uint8_t> span, const ComputedPlaneLayout& layout, const VideoFrameHaiku::Plane& plane, size_t rowBytes, size_t rows)
{
    size_t stride = layout.destinationStride ? layout.destinationStride : layout.sourceWidthBytes;
    rowBytes = std::min(rowBytes, layout.sourceWidthBytes ? layout.sourceWidthBytes : rowBytes);
    rows = std::min(rows, layout.sourceHeight ? layout.sourceHeight : rows);
    if (!rows)
        return true;
    if (layout.destinationOffset + stride * (rows - 1) + rowBytes > span.size())
        return false;
    copyRows(span.data() + layout.destinationOffset, stride, plane.data, plane.stride, rowBytes, rows);
    return true;
}

RefPtr<VideoFrame> VideoFrame::createNV12(std::span<const uint8_t> span, size_t width, size_t height, const ComputedPlaneLayout& planeY, const ComputedPlaneLayout& planeUV, PlatformVideoColorSpace&& colorSpace)
{
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::NV12, IntSize(width, height), MediaTime::zeroTime(), false, Rotation::None, WTF::move(colorSpace));
    if (!copyPlaneFrom(span, planeY, frame->plane(0), width, height)
        || !copyPlaneFrom(span, planeUV, frame->plane(1), 2 * ((width + 1) / 2), (height + 1) / 2))
        return nullptr;
    return frame;
}

RefPtr<VideoFrame> VideoFrame::createRGBA(std::span<const uint8_t> span, size_t width, size_t height, const ComputedPlaneLayout& plane, PlatformVideoColorSpace&& colorSpace)
{
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::RGBA, IntSize(width, height), MediaTime::zeroTime(), false, Rotation::None, WTF::move(colorSpace));
    if (!copyPlaneFrom(span, plane, frame->plane(0), 4 * width, height))
        return nullptr;
    return frame;
}

RefPtr<VideoFrame> VideoFrame::createBGRA(std::span<const uint8_t> span, size_t width, size_t height, const ComputedPlaneLayout& plane, PlatformVideoColorSpace&& colorSpace)
{
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::BGRA, IntSize(width, height), MediaTime::zeroTime(), false, Rotation::None, WTF::move(colorSpace));
    if (!copyPlaneFrom(span, plane, frame->plane(0), 4 * width, height))
        return nullptr;
    return frame;
}

RefPtr<VideoFrame> VideoFrame::createI420(std::span<const uint8_t> span, size_t width, size_t height, const ComputedPlaneLayout& planeY, const ComputedPlaneLayout& planeU, const ComputedPlaneLayout& planeV, PlatformVideoColorSpace&& colorSpace)
{
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::I420, IntSize(width, height), MediaTime::zeroTime(), false, Rotation::None, WTF::move(colorSpace));
    size_t chromaWidth = (width + 1) / 2;
    size_t chromaHeight = (height + 1) / 2;
    if (!copyPlaneFrom(span, planeY, frame->plane(0), width, height)
        || !copyPlaneFrom(span, planeU, frame->plane(1), chromaWidth, chromaHeight)
        || !copyPlaneFrom(span, planeV, frame->plane(2), chromaWidth, chromaHeight))
        return nullptr;
    return frame;
}

RefPtr<VideoFrame> VideoFrame::createI420A(std::span<const uint8_t> span, size_t width, size_t height, const ComputedPlaneLayout& planeY, const ComputedPlaneLayout& planeU, const ComputedPlaneLayout& planeV, const ComputedPlaneLayout&, PlatformVideoColorSpace&& colorSpace)
{
    // The alpha plane is dropped: nothing on Haiku composites a transparent video.
    return createI420(span, width, height, planeY, planeU, planeV, WTF::move(colorSpace));
}

RefPtr<VideoFrame> VideoFrame::createFromPixelBuffer(Ref<PixelBuffer>&& pixelBuffer, PlatformVideoColorSpace&& colorSpace)
{
    auto& format = pixelBuffer->format();
    VideoFrameHaiku::Format frameFormat;
    switch (format.pixelFormat) {
    case PixelFormat::RGBA8:
        frameFormat = VideoFrameHaiku::Format::RGBA;
        break;
    case PixelFormat::BGRA8:
    case PixelFormat::BGRX8:
        frameFormat = VideoFrameHaiku::Format::BGRA;
        break;
    default:
        return nullptr;
    }
    auto size = pixelBuffer->size();
    auto frame = VideoFrameHaiku::create(frameFormat, size, MediaTime::zeroTime(), false, Rotation::None, WTF::move(colorSpace));
    auto bytes = pixelBuffer->bytes();
    size_t rowBytes = 4 * size.width();
    if (bytes.size() < rowBytes * size.height())
        return nullptr;
    copyRows(bytes.data(), rowBytes, frame->plane(0).data, frame->plane(0).stride, rowBytes, size.height());
    frame->setIsOpaque(format.pixelFormat == PixelFormat::BGRX8);
    return frame;
}

RefPtr<VideoFrame> VideoFrame::fromNativeImage(NativeImage& image)
{
#if USE(SKIA)
    auto platformImage = image.platformImage();
    if (!platformImage)
        return nullptr;
    auto size = image.size();
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::BGRA, size, MediaTime::zeroTime());
    auto info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    if (!platformImage->readPixels(image.grContext(), info, frame->plane(0).data, frame->plane(0).stride, 0, 0))
        return nullptr;
    frame->setIsOpaque(platformImage->isOpaque());
    return frame;
#else
    UNUSED_PARAM(image);
    return nullptr;
#endif
}

void VideoFrame::copyTo(std::span<uint8_t> destination, VideoPixelFormat pixelFormat, Vector<ComputedPlaneLayout>&& computedPlaneLayouts, CopyCallback&& callback)
{
    auto& self = downcast<VideoFrameHaiku>(*this);
    std::optional<VideoFrameHaiku::Format> format;
    switch (pixelFormat) {
    case VideoPixelFormat::I420:
    case VideoPixelFormat::I420A:
        format = VideoFrameHaiku::Format::I420;
        break;
    case VideoPixelFormat::NV12:
        format = VideoFrameHaiku::Format::NV12;
        break;
    case VideoPixelFormat::BGRA:
    case VideoPixelFormat::BGRX:
        format = VideoFrameHaiku::Format::BGRA;
        break;
    case VideoPixelFormat::RGBA:
    case VideoPixelFormat::RGBX:
        format = VideoFrameHaiku::Format::RGBA;
        break;
    case VideoPixelFormat::I422:
    case VideoPixelFormat::I444:
        break;
    }
    if (!format) {
        callback({ });
        return;
    }

    RefPtr<VideoFrameHaiku> source = &self;
    if (self.format() != *format)
        source = self.convertedTo(*format);
    if (!source || computedPlaneLayouts.size() < source->planeCount()) {
        callback({ });
        return;
    }

    // Plane sizes in bytes of a row and rows, for the copy rectangle of each plane.
    Vector<PlaneLayout> planeLayouts;
    for (unsigned index = 0; index < source->planeCount(); ++index) {
        auto& layout = computedPlaneLayouts[index];
        auto& plane = source->plane(index);
        size_t stride = layout.destinationStride ? layout.destinationStride : layout.sourceWidthBytes;
        if (layout.sourceHeight) {
            size_t end = layout.destinationOffset + stride * (layout.sourceHeight - 1) + layout.sourceWidthBytes;
            if (end > destination.size()) {
                callback({ });
                return;
            }
            copyRows(plane.data + layout.sourceTop * plane.stride + layout.sourceLeftBytes, plane.stride,
                destination.data() + layout.destinationOffset, stride, layout.sourceWidthBytes, layout.sourceHeight);
        }
        planeLayouts.append({ layout.destinationOffset, stride });
    }
    callback(WTF::move(planeLayouts));
}

RefPtr<NativeImage> VideoFrame::copyNativeImage() const
{
    return downcast<VideoFrameHaiku>(*this).nativeImage();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(HAIKU)
