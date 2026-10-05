/*
 * Copyright (C) 2014 Haiku, Inc.
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

#include "config.h"
#include "ColorSpace.h"
#include "ShareableBitmap.h"

#include "BitmapImage.h"
#include "GraphicsContext.h"
#include "GraphicsContextHaiku.h"
#include "IntRect.h"

#include <Bitmap.h>
#include <View.h>
#include <cmath>
#include <limits>

namespace WebCore {

namespace {

// BBitmap borrows a client area's address. Keep the mapping alive for every
// native image reference, including references that outlive ShareableBitmap.
class SharedBitmapImage final : public BitmapRef {
public:
    SharedBitmapImage(Ref<SharedMemory>&& memory, const IntRect& bounds, uint32 flags, int32 bytesPerRow)
        : BitmapRef(memory->area(), 0, bounds, flags, B_RGBA32, bytesPerRow)
        , m_memory(WTF::move(memory))
    {
    }

private:
    Ref<SharedMemory> m_memory;
};

class SharedBitmapGraphicsContext final : public GraphicsContextHaiku {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(SharedBitmapGraphicsContext);
public:
    SharedBitmapGraphicsContext(BView* view, Ref<BitmapRef>&& image)
        : GraphicsContextHaiku(view)
        , m_image(WTF::move(image))
    {
    }

    ~SharedBitmapGraphicsContext() override
    {
        // Finish app_server writes before publishing the shared backing store.
        platformContext()->Sync();
        m_image->Unlock();
    }

private:
    Ref<BitmapRef> m_image;
};

bool memoryIsWritable(const SharedMemory& memory)
{
    area_info info;
    return get_area_info(area_for(memory.mutableSpan().data()), &info) == B_OK
        && (info.protection & B_WRITE_AREA);
}

} // namespace

std::unique_ptr<GraphicsContext> ShareableBitmap::createGraphicsContext()
{
    if (!memoryIsWritable(m_sharedMemory))
        return nullptr;
    Ref<BitmapRef> image = adoptRef(*new SharedBitmapImage(m_sharedMemory.copyRef(), bounds(), B_BITMAP_ACCEPTS_VIEWS, bytesPerRow()));
    if (!image->IsValid() || !image->Lock())
        return nullptr;

    auto surface = std::make_unique<BView>(image->Bounds(), "WebKit shared bitmap", 0, 0);
    image->AddChild(surface.get());
    if (!surface->Looper()) {
        image->Unlock();
        return nullptr;
    }
    auto* view = surface.release(); // BBitmap owns its child views.

    return makeUnique<SharedBitmapGraphicsContext>(view, WTF::move(image));
}

void ShareableBitmap::paint(GraphicsContext& context, const IntPoint& dstPoint, const IntRect& srcRect)
{
    paint(context, 1, dstPoint, srcRect);
}

void ShareableBitmap::paint(GraphicsContext& context, float scaleFactor, const IntPoint& dstPoint, const IntRect& srcRect)
{
    if (!context.hasPlatformContext() || srcRect.isEmpty() || !std::isfinite(scaleFactor) || scaleFactor <= 0)
        return;
    auto image = createPlatformImage(DontCopyBackingStore);
    if (!image)
        return;
    FloatRect destRect(dstPoint, srcRect.size());
    FloatRect srcRectScaled(srcRect);
    srcRectScaled.scale(scaleFactor);
    static_cast<GraphicsContextHaiku&>(context).drawBitmap(image.get(), destRect, srcRectScaled,
        ImagePaintingOptions { context.compositeOperation(), context.imageInterpolationQuality() });
    context.platformContext()->Sync();
}

PlatformImagePtr ShareableBitmap::createBasePlatformImage(BackingStoreCopy copyBehavior, ShouldInterpolate shouldInterpolate)
{
    return createPlatformImage(copyBehavior, shouldInterpolate);
}

PlatformImagePtr ShareableBitmap::createPlatformImage(BackingStoreCopy copyBehavior, ShouldInterpolate)
{
    if (copyBehavior == DontCopyBackingStore && memoryIsWritable(m_sharedMemory)) {
        auto image = adoptRef(*new SharedBitmapImage(m_sharedMemory.copyRef(), bounds(), 0, bytesPerRow()));
        if (image->IsValid())
            return image;
    }
    // Haiku's app_server clones bitmap areas with write access. Read-only
    // capabilities stay uncloneable and are displayed through private copies.
    auto image = adoptRef(*new BitmapRef(bounds(), 0, B_RGBA32, bytesPerRow()));
    if (!image->IsValid() || image->BitsLength() <= 0 || static_cast<size_t>(image->BitsLength()) != sizeInBytes())
        return nullptr;
    memcpy(image->Bits(), span().data(), sizeInBytes());
    return image;
}

RefPtr<Image> ShareableBitmap::createImage()
{
    WebCore::PlatformImagePtr surface = createPlatformImage(DontCopyBackingStore);
    if (!surface)
        return nullptr;

    return BitmapImage::create(std::move(surface));
}

void ShareableBitmap::setOwnershipOfMemory(const ProcessIdentity&)
{
    // This hook attributes Mach graphics-ledger usage on CG ports. Haiku
    // retains native area accounting and the existing descriptor protections.
}

CheckedUint32 ShareableBitmapConfiguration::calculateBitsPerComponent(WebCore::PixelFormat fmt, const ColorSpace& colorSpace)
{
    return (calculateBytesPerPixel(fmt, colorSpace) / 4) * 8;
}

CheckedUint32 ShareableBitmapConfiguration::calculateBytesPerRow(const IntSize& size,
    PixelFormat pixelFormat, const ColorSpace& colorSpace)
{
    if (size.isEmpty())
        return WTF::ResultOverflowed;
    auto bytes = calculateBytesPerPixel(pixelFormat, colorSpace) * size.width();
    if (bytes.hasOverflowed() || bytes.value() > std::numeric_limits<int32_t>::max())
        return WTF::ResultOverflowed;
    return bytes;
}

CheckedUint32 ShareableBitmapConfiguration::calculateBytesPerPixel(PixelFormat pixelFormat, const ColorSpace&)
{
    switch (pixelFormat) {
    case PixelFormat::RGBX8:
    case PixelFormat::RGBA8:
    case PixelFormat::BGRX8:
    case PixelFormat::BGRA8:
        return 4;
    default:
        return WTF::ResultOverflowed;
    }
}

ColorSpace ShareableBitmapConfiguration::validateColorSpace(const ColorSpace&)
{
    return ColorSpace::SRGB();
}

}
