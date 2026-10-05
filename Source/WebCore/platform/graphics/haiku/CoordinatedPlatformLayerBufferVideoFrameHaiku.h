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

#if ENABLE(VIDEO) && USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)

#include "BitmapTexturePool.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedPlatformLayerBufferYUV.h"
#include "TextureMapper.h"
#include "VideoFrameHaiku.h"

namespace WebCore {

// An I420 video frame (camera, screen or WebRTC picture) for the compositor:
// three single-channel textures it converts to RGB as it draws, as for the
// Raspberry Pi's decoder (CoordinatedPlatformLayerBufferPlanesHaiku).
class CoordinatedPlatformLayerBufferVideoFrameHaiku final : public CoordinatedPlatformLayerBuffer {
public:
    explicit CoordinatedPlatformLayerBufferVideoFrameHaiku(Ref<VideoFrameHaiku>&& frame)
        : CoordinatedPlatformLayerBuffer(Type::NativeImage, frame->size(), { }, nullptr)
        , m_frame(WTF::move(frame))
    {
        ASSERT(m_frame->format() == VideoFrameHaiku::Format::I420);
    }

    void paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& modelViewMatrix, float opacity) final
    {
        if (!m_buffer && m_frame) {
            IntSize chromaSize((m_size.width() + 1) / 2, (m_size.height() + 1) / 2);
            Vector<RefPtr<BitmapTexture>, 4> textures;
            auto& pool = BitmapTexturePool::singleton();
            auto upload = [&](const VideoFrameHaiku::Plane& plane, const IntSize& size) {
                Ref texture = pool.acquireTexture(size, BitmapTexture::Flags::SingleChannel);
                texture->updateContents(plane.data, IntRect({ }, size), { }, plane.stride, PixelFormat::RGBA8);
                textures.append(WTF::move(texture));
            };
            upload(m_frame->plane(0), m_size);
            upload(m_frame->plane(1), chromaSize);
            upload(m_frame->plane(2), chromaSize);

            auto& colorSpace = m_frame->colorSpace();
            bool bt709 = m_size.height() >= 720;
            if (colorSpace.matrix)
                bt709 = *colorSpace.matrix == PlatformVideoMatrixCoefficients::Bt709;
            m_buffer = CoordinatedPlatformLayerBufferYUV::create(CoordinatedPlatformLayerBufferYUV::Format::YUV420, 3, WTF::move(textures),
                { 0, 1, 2, 0 }, { 0, 0, 0, 0 },
                bt709 ? CoordinatedPlatformLayerBufferYUV::YuvToRgbColorSpace::Bt709 : CoordinatedPlatformLayerBufferYUV::YuvToRgbColorSpace::Bt601,
                CoordinatedPlatformLayerBufferYUV::TransferFunction::Bt709, m_size, m_flags, nullptr);
            m_frame = nullptr;
        }
        if (m_buffer)
            m_buffer->paintToTextureMapper(textureMapper, targetRect, modelViewMatrix, opacity);
    }

private:
    RefPtr<VideoFrameHaiku> m_frame;
    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_buffer;
};

} // namespace WebCore

#endif
