/*
 * Copyright (C) 2008 Alp Toker <alp@atoker.com>
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
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "FontCustomPlatformData.h"

#include "FontPlatformData.h"
#include "NotImplemented.h"
#include "OpenTypeUtilities.h"
#include "SharedBuffer.h"
#include <limits>
#include <wtf/UUID.h>

#include "wtf/RefPtr.h"

namespace WebCore {

FontCustomPlatformData::~FontCustomPlatformData()
{
	m_font.UnloadFont();
	delete_area(m_area);
}

FontPlatformData FontCustomPlatformData::fontPlatformData(const FontDescription& description, const FontCreationContext& context)
{
    return FontPlatformData(m_font, description, this, context.metricsOverrides());
}

RefPtr<FontCustomPlatformData> FontCustomPlatformData::create(SharedBuffer& buffer, const String& itemInCollection)
{
	// app_server identifies private faces by their internal family/style names.
	// Two downloads (including different subsets) may have the same names, so
	// give each native face its own identity while retaining the original data
	// for WebCore and IPC. Use the upstream OpenType name-table rewriter.
	Vector<uint8_t> nativeData;
	if (!renameFont(buffer, WTF::UUID::createVersion4().toString(), nativeData)
		|| nativeData.size() > std::numeric_limits<size_t>::max() - (B_PAGE_SIZE - 1))
		return nullptr;
	void* sharedData = nullptr;
	// TODO is it possible to create a SharedBuffer so that it is allocated in a clonable area already?
	// It would save a copy here.
	auto areaSize = (nativeData.size() + B_PAGE_SIZE - 1) & ~(static_cast<size_t>(B_PAGE_SIZE) - 1);
	area_id area = create_area("web font", &sharedData, B_ANY_ADDRESS, areaSize, B_NO_LOCK, B_WRITE_AREA | B_READ_AREA | B_CLONEABLE_AREA);
	if (area < B_OK)
		return nullptr;

	memcpy(sharedData, nativeData.span().data(), nativeData.size());
	BFont font;

	status_t result = font.LoadFont(area, nativeData.size(), 0);

	if (result != B_OK) { // B_NOT_SUPPORTED returned for r1beta4 fallback path
		delete_area(area);
		return nullptr;
	}

    FontPlatformData::CreationData creationData = { buffer, itemInCollection };
    return adoptRef(*new FontCustomPlatformData(area, font, std::move(creationData)));
}

FontCustomPlatformSerializedData FontCustomPlatformData::serializedData() const
{
    return { creationData.fontFaceData, creationData.itemInCollection, m_renderingResourceIdentifier };
}

std::optional<Ref<FontCustomPlatformData>> FontCustomPlatformData::tryMakeFromSerializationData(FontCustomPlatformSerializedData&& data, bool shouldUseLockdownParser)
{
    auto font = shouldUseLockdownParser ? createMemorySafe(data.fontFaceData, data.itemInCollection)
        : create(data.fontFaceData, data.itemInCollection);
    if (!font)
        return std::nullopt;
    font->m_renderingResourceIdentifier = data.renderingResourceIdentifier;
    return font.releaseNonNull();
}

RefPtr<FontCustomPlatformData> FontCustomPlatformData::createMemorySafe(SharedBuffer&, const String&)
{
    return nullptr;
}

bool FontCustomPlatformData::supportsFormat(const String& format)
{
    return equalIgnoringASCIICase(format, ASCIILiteral::fromLiteralUnsafe("truetype"))
        || equalIgnoringASCIICase(format, ASCIILiteral::fromLiteralUnsafe("opentype"))
#if USE(WOFF2)
        || equalIgnoringASCIICase(format, "woff2"_s)
#endif
        || equalIgnoringASCIICase(format, ASCIILiteral::fromLiteralUnsafe("woff"))
    ;
}

bool FontCustomPlatformData::supportsTechnology(const FontTechnology&)
{
    // FIXME: define supported technologies for this platform (webkit.org/b/256310).
    notImplemented();
    return true;
}
}
