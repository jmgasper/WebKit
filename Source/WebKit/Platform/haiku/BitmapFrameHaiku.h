/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <WebCore/IntSize.h>
#include <WebCore/ShareableBitmap.h>
#include <cmath>
#include <optional>

namespace WebKit {

inline std::optional<WebCore::IntSize> bitmapFrameSizeHaiku(const WebCore::IntSize& viewSize, float scale)
{
    if (viewSize.isEmpty() || !std::isfinite(scale) || scale <= 0 || scale > 8)
        return std::nullopt;
    double width = std::ceil(viewSize.width() * static_cast<double>(scale));
    double height = std::ceil(viewSize.height() * static_cast<double>(scale));
    // Bound both native BBitmap geometry and memory accepted from WebContent.
    constexpr int maximumDimension = 8192;
    if (width < 1 || height < 1 || width > maximumDimension || height > maximumDimension)
        return std::nullopt;
    return WebCore::IntSize { static_cast<int>(width), static_cast<int>(height) };
}

inline bool bitmapFrameHandleIsValidHaiku(const WebCore::IntSize& viewSize, float scale, WebCore::ShareableBitmapHandle& handle)
{
    auto expectedSize = bitmapFrameSizeHaiku(viewSize, scale);
    if (!expectedSize)
        return false;
    const auto& configuration = handle.configuration();
    auto expectedStride = static_cast<size_t>(expectedSize->width()) * 4;
    auto expectedBytes = expectedStride * expectedSize->height();
    return configuration.size() == *expectedSize && configuration.bytesPerRow() == expectedStride
        && configuration.bytesPerPixel() == 4 && configuration.bitsPerComponent() == 8
        && configuration.colorSpace() == WebCore::ColorSpace::SRGB()
        && (configuration.pixelFormat() == WebCore::PixelFormat::RGBA8 || configuration.pixelFormat() == WebCore::PixelFormat::BGRA8)
        && handle.handle().size() == expectedBytes;
}

} // namespace WebKit
