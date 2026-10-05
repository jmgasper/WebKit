/*
 * Copyright (C) 2007 Ryan Leavengood <leavengood@gmail.com>
 *
 * All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"
#include "Icon.h"

#include "GraphicsContext.h"
#include "FloatRect.h"
#include "NativeImage.h"
#include <Entry.h>
#include <NodeInfo.h>
#if USE(SKIA)
#include <Bitmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

Icon::Icon(RefPtr<NativeImage>&& image)
    : m_image(WTF::move(image))
{
}

Icon::~Icon() = default;

RefPtr<Icon> Icon::create(RefPtr<NativeImage>&& image)
{
    if (!image || image->size().isEmpty())
        return nullptr;
    return adoptRef(*new Icon(WTF::move(image)));
}

void Icon::paint(GraphicsContext& context, const FloatRect& destination)
{
    if (!destination.isEmpty())
        context.drawNativeImage(*m_image, destination, FloatRect({ }, m_image->size()));
}

RefPtr<Icon> Icon::createIconForFiles(const Vector<String>& filenames)
{
    if (filenames.isEmpty() || filenames[0].isEmpty() || filenames[0].contains('\0'))
        return nullptr;
    BEntry entry(filenames[0].utf8().legacyCStringPointer(), true);
    entry_ref reference;
    if (entry.GetRef(&reference) != B_OK)
        return nullptr;
#if USE(HAIKU_GRAPHICS)
    PlatformImagePtr bitmap = adoptRef(*new BitmapRef(BRect(0, 0, 31, 31), B_RGBA32, false));
    if (!bitmap->IsValid() || BNodeInfo::GetTrackerIcon(&reference, bitmap.get(), B_LARGE_ICON) != B_OK)
        return nullptr;
    return create(NativeImage::create(WTF::move(bitmap)));
#else
    // Tracker still draws the icon into a BBitmap; only the image it becomes
    // afterwards differs. B_RGBA32 holds straight alpha, hence kUnpremul.
    BBitmap bitmap(BRect(0, 0, 31, 31), B_RGBA32, false);
    if (!bitmap.IsValid() || BNodeInfo::GetTrackerIcon(&reference, &bitmap, B_LARGE_ICON) != B_OK)
        return nullptr;
    auto info = SkImageInfo::Make(bitmap.Bounds().IntegerWidth() + 1, bitmap.Bounds().IntegerHeight() + 1,
        kBGRA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, bitmap.Bits(), bitmap.BytesPerRow());
    auto image = SkImages::RasterFromPixmapCopy(pixmap);
    if (!image)
        return nullptr;
    return create(NativeImage::create(WTF::move(image)));
#endif
}

} // namespace WebCore
