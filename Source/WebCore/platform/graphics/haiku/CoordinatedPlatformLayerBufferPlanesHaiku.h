/*
 * Copyright (C) 2026 Haiku, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)

#include "BitmapTexturePool.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedPlatformLayerBufferYUV.h"
#include "MediaDecoderSelectionHaiku.h"
#include "TextureMapper.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// A video picture as the Raspberry Pi's decoder hands it out, three planes
// (I420), for the compositor: it uploads them into single-channel textures
// and converts to RGB as it draws. That is a third of the bytes an RGB
// picture would upload and no conversion on the CPU, which on the Pi matters
// twice: the firmware's decoder slows the ARM's memory down while it works.
class CoordinatedPlatformLayerBufferPlanesHaiku final : public CoordinatedPlatformLayerBuffer {
public:
    CoordinatedPlatformLayerBufferPlanesHaiku(sk_sp<SkData>&& planes, const AddOnVideoDecoderHaiku::I420Layout& layout)
        // Not a native image, but nothing looks at this type but the type
        // traits of the classes that have it.
        : CoordinatedPlatformLayerBuffer(Type::NativeImage, IntSize(layout.width, layout.height), { }, nullptr)
        , m_planes(WTF::move(planes))
        , m_layout(layout)
    {
    }

    void paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& modelViewMatrix, float opacity) final
    {
        if (!m_buffer && m_planes) {
            auto* bytes = static_cast<const uint8_t*>(m_planes->data());
            IntSize chromaSize((m_layout.width + 1) / 2, (m_layout.height + 1) / 2);
            Vector<RefPtr<BitmapTexture>, 4> textures;
            auto& pool = BitmapTexturePool::singleton();
            auto upload = [&](const uint8_t* plane, const IntSize& size, size_t rowBytes) {
                Ref texture = pool.acquireTexture(size, BitmapTexture::Flags::SingleChannel);
                texture->updateContents(plane, IntRect({ }, size), { }, rowBytes, PixelFormat::RGBA8);
                textures.append(WTF::move(texture));
            };
            upload(bytes, m_size, m_layout.lumaRowBytes);
            upload(bytes + m_layout.chromaOffset(1), chromaSize, m_layout.lumaRowBytes / 2);
            upload(bytes + m_layout.chromaOffset(2), chromaSize, m_layout.lumaRowBytes / 2);
            m_buffer = CoordinatedPlatformLayerBufferYUV::create(CoordinatedPlatformLayerBufferYUV::Format::YUV420, 3, WTF::move(textures),
                { 0, 1, 2, 0 }, { 0, 0, 0, 0 },
                m_layout.bt709() ? CoordinatedPlatformLayerBufferYUV::YuvToRgbColorSpace::Bt709 : CoordinatedPlatformLayerBufferYUV::YuvToRgbColorSpace::Bt601,
                CoordinatedPlatformLayerBufferYUV::TransferFunction::Bt709, m_size, m_flags, nullptr);
            // The decoder may use the memory for another picture.
            m_planes = nullptr;
        }
        if (m_buffer)
            m_buffer->paintToTextureMapper(textureMapper, targetRect, modelViewMatrix, opacity);
    }

private:
    sk_sp<SkData> m_planes;
    AddOnVideoDecoderHaiku::I420Layout m_layout;
    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_buffer;
};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER) && USE(SKIA)
