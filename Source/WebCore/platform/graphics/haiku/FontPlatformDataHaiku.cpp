/*
 * Copyright (C) 2009 Maxime Simon <simon.maxime@gmail.com>
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
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
#include "FontDescription.h"
#include "FontCustomPlatformData.h"
#include "FontPlatformData.h"
#include "SharedBuffer.h"
#include "NotImplemented.h"
#include "RenderStyleConstants.h"

#include <String.h>
#include <cmath>

#include <wtf/text/AtomString.h>
#include <wtf/text/CString.h>

namespace WebCore {
font_family FontPlatformData::m_FallbackSansSerifFontFamily= "Noto Sans";
font_family FontPlatformData::m_FallbackSerifFontFamily = "Noto Serif";
font_family FontPlatformData::m_FallbackFixedFontFamily = "Noto Mono";
font_family FontPlatformData::m_FallbackStandardFontFamily = "Noto Sans";

void
FontPlatformData::findMatchingFontFamily(const AtomString& familyName, font_family& fontFamily)
{
    CString familyNameUTF8 = familyName.string().utf8();
    if (BFont().SetFamilyAndStyle(familyNameUTF8.data(), 0) == B_OK)
        strlcpy(fontFamily, familyNameUTF8.data(), B_FONT_FAMILY_LENGTH);
    else {
        // If no font family is found for the given name, we use a generic font.

        AtomString lowercaseFamilyName = familyName.convertToASCIILowercase();

        // This is a known foundry with its name in some families, not necessarily monospaced
        ASCIILiteral monotype = ASCIILiteral::fromLiteralUnsafe("monotype ");
        if (lowercaseFamilyName.startsWith(monotype))
            lowercaseFamilyName = AtomString(lowercaseFamilyName.impl(), monotype.length(), lowercaseFamilyName.length() - monotype.length());

        if (lowercaseFamilyName.contains(ASCIILiteral::fromLiteralUnsafe("mono"))
            || lowercaseFamilyName.contains(ASCIILiteral::fromLiteralUnsafe("consol")))
            strlcpy(fontFamily, m_FallbackFixedFontFamily, B_FONT_FAMILY_LENGTH);
        else if (lowercaseFamilyName.contains(ASCIILiteral::fromLiteralUnsafe("sans")))
            strlcpy(fontFamily, m_FallbackSansSerifFontFamily, B_FONT_FAMILY_LENGTH);
        else if (lowercaseFamilyName.contains(ASCIILiteral::fromLiteralUnsafe("serif")))
            strlcpy(fontFamily, m_FallbackSerifFontFamily, B_FONT_FAMILY_LENGTH);
        else {
            // This is the fallback font.
            strlcpy(fontFamily, m_FallbackStandardFontFamily, B_FONT_FAMILY_LENGTH);
        }
    }
}

static void findMatchingFontStyle(const font_family& fontFamily, bool bold, bool oblique, font_style* fontStyle)
{
    int32 numStyles = count_font_styles(const_cast<char*>(fontFamily));

    for (int i = 0; i < numStyles; i++) {
        if (get_font_style(const_cast<char*>(fontFamily), i, fontStyle) == B_OK) {
            BString styleName(*fontStyle);
            if ((oblique && bold) && (styleName == "Bold Italic" || styleName == "Bold Oblique"))
                return;
            if ((oblique && !bold) && (styleName == "Italic" || styleName == "Oblique"))
                return;
            if ((!oblique && bold) && styleName == "Bold")
                return;
            if ((!oblique && !bold) && (styleName == "Roman" || styleName == "Book"
                || styleName == "Condensed" || styleName == "Regular" || styleName == "Medium")) {
                return;
            }
        }
    }

    // Didn't find matching style
    memset(*fontStyle, 0, sizeof(font_style));
}

// #pragma mark -

FontPlatformData::FontPlatformData(const FontDescription& fontDescription, const AtomString& familyName)
    : FontPlatformData(fontDescription.usedSize(), false, false, fontDescription.orientation(), fontDescription.widthVariant(), fontDescription.textRenderingMode())
{
    m_font = std::make_unique<BFont>();
    m_font->SetSize(fontDescription.usedSize());

    font_family fontFamily;
    findMatchingFontFamily(familyName, fontFamily);

    font_style fontStyle;
    findMatchingFontStyle(fontFamily, fontDescription.weight() == boldWeightValue(),
        fontDescription.fontStyleSlope() != std::nullopt, &fontStyle);

    m_font->SetFamilyAndStyle(fontFamily, fontStyle);
}


FontPlatformData::FontPlatformData(const BFont& font, const FontDescription& fontDescription)
    : FontPlatformData(font, fontDescription, nullptr)
{
}

FontPlatformData::FontPlatformData(const BFont& font, const FontDescription& fontDescription, const FontCustomPlatformData* custom, const FontMetricsOverrides& overrides)
    : FontPlatformData(fontDescription.usedSize(), false, false, fontDescription.orientation(), fontDescription.widthVariant(), fontDescription.textRenderingMode(), overrides, custom)
{
    m_font = std::make_unique<BFont>(font);
    m_font->SetSize(fontDescription.usedSize());

    // Private downloaded faces are not entries in the installed-font catalog.
    if (custom)
        return;

    font_family fontFamily;
    font_style fontStyle;

    font.GetFamilyAndStyle(&fontFamily, &fontStyle);

    findMatchingFontStyle(fontFamily, fontDescription.weight() == boldWeightValue(),
        fontDescription.fontStyleSlope() != std::nullopt, &fontStyle);

    m_font->SetFamilyAndStyle(fontFamily, fontStyle);
}


void FontPlatformData::updateSize(float size)
{
	m_metadata.pointSize = size;
	// The default copy constructor does not create a copy of the underlying font!
	// Here is a weird time to do it, bu that's what FreeType and Core Text do, so let's follow
	// them. Some kind of copy-on-write optimization.
	// Otherwise, all objects that share the same BFont because they are copies of each other would
	// all be affected by the size update.
	m_font = std::make_shared<BFont>(font());
	m_font->SetSize(size);
}


unsigned FontPlatformData::hash() const
{
	float tmp;
	unsigned result = 0;

	result ^= m_font->FamilyAndStyle();
	result ^= m_font->Spacing() << 24;
	result ^= m_font->Encoding() << 16;
	result ^= m_font->Face();

	tmp = m_font->Size();
	result ^= *(unsigned*)&tmp;

	tmp = m_font->Shear();
	result ^= *(unsigned*)&tmp;

	tmp = m_font->Rotation();
	result ^= *(unsigned*)&tmp;

	tmp = m_font->FalseBoldWidth();
	result ^= *(unsigned*)&tmp;

	return result;
}


bool FontPlatformData::platformIsEqual(const FontPlatformData& other) const
{
	if (m_font == nullptr && other.m_font == nullptr)
		return true;
	if (m_font == nullptr || other.m_font == nullptr)
		return false;
	return (*m_font == *other.m_font);
}

bool FontPlatformData::isFixedPitch() const
{
	if (m_font == nullptr)
		return false;
	return m_font->Spacing() == B_FIXED_SPACING;
}

void
FontPlatformData::SetFallBackSerifFont(const BString& font)
{
	strlcpy(m_FallbackSerifFontFamily, font.String(), B_FONT_FAMILY_LENGTH);
}

void
FontPlatformData::SetFallBackSansSerifFont(const BString& font)
{
	strlcpy(m_FallbackSansSerifFontFamily, font.String(), B_FONT_FAMILY_LENGTH);
}

void
FontPlatformData::SetFallBackFixedFont(const BString& font)
{
	strlcpy(m_FallbackFixedFontFamily, font.String(), B_FONT_FAMILY_LENGTH);
}

void
FontPlatformData::SetFallBackStandardFont(const BString& font)
{
	strlcpy(m_FallbackStandardFontFamily, font.String(), B_FONT_FAMILY_LENGTH);
}

RefPtr<SharedBuffer> FontPlatformData::openTypeTable(uint32_t table) const
{
	UNUSED_PARAM(table);
	notImplemented();
	return nullptr;
}

String FontPlatformData::description() const
{
    return familyName();
}

String FontPlatformData::familyName() const
{
	if (!m_font)
		return emptyString();
	font_family family;
	font_style style;
	m_font->GetFamilyAndStyle(&family, &style);
	return String::fromUTF8(family);
}

static FontPlatformSerializedData serializeFontHaiku(const BFont* font)
{
    if (!font)
        return { };
    font_family family;
    font_style style;
    font->GetFamilyAndStyle(&family, &style);
    return { String::fromUTF8(family), String::fromUTF8(style), font->Shear(), font->Rotation(),
        font->FalseBoldWidth(), font->Flags(), font->Face(), font->Spacing(), font->Encoding() };
}

static bool validFontDataHaiku(const FontMetadata& metadata, const FontPlatformSerializedData& data)
{
    if (!std::isfinite(metadata.pointSize) || metadata.pointSize < 0 || metadata.pointSize > maximumAllowedFontSize
        || !std::isfinite(data.shear) || data.shear < 0 || data.shear > 180
        || !std::isfinite(data.rotation) || data.rotation < 0 || data.rotation > 360
        || !std::isfinite(data.falseBoldWidth) || data.falseBoldWidth < 0 || data.falseBoldWidth > maximumAllowedFontSize
        || data.spacing > B_FIXED_SPACING || data.encoding != B_UNICODE_UTF8
        || (data.flags & ~(B_DISABLE_ANTIALIASING | B_FORCE_ANTIALIASING)) || (data.face & ~0x3ff))
        return false;
    auto family = data.family.utf8();
    auto style = data.style.utf8();
    return !data.family.isEmpty() && !data.family.contains('\0') && !data.style.contains('\0')
        && family.length() < B_FONT_FAMILY_LENGTH && style.length() < B_FONT_STYLE_LENGTH;
}

static bool restoreFontHaiku(BFont& font, const FontMetadata& metadata, const FontPlatformSerializedData& data, bool custom)
{
    if (!validFontDataHaiku(metadata, data))
        return false;
    if (!custom && font.SetFamilyAndStyle(data.family.utf8().legacyCStringPointer(), data.style.utf8().legacyCStringPointer()) != B_OK)
        return false;
    font.SetSize(metadata.pointSize);
    font.SetShear(data.shear);
    font.SetRotation(data.rotation);
    font.SetFalseBoldWidth(data.falseBoldWidth);
    font.SetFlags(data.flags);
    font.SetSpacing(data.spacing);
    font.SetEncoding(data.encoding);
    // Family/style restoration already selects the installed face. Changing
    // family or face on a private font can detach it from its loaded data.
    if (!custom && font.Face() != data.face)
        font.SetFace(data.face);
    return true;
}

FontPlatformData::IPCData FontPlatformData::toIPCData() const
{
    auto fontData = serializeFontHaiku(font());
    if (auto* data = creationData())
        return CustomFontCreationData { { data->fontFaceData->span() }, data->itemInCollection, WTF::move(fontData) };
    return fontData;
}

std::optional<FontPlatformData> FontPlatformData::fromIPCData(const FontMetadata& metadata, IPCData&& data)
{
    return WTF::switchOn(data,
        [&] (const FontPlatformSerializedData& data) -> std::optional<FontPlatformData> {
            FontPlatformData result(metadata);
            result.m_font = std::make_shared<BFont>();
            if (!restoreFontHaiku(*result.m_font, metadata, data, false))
                return std::nullopt;
            return result;
        },
        [&] (CustomFontCreationData& data) -> std::optional<FontPlatformData> {
            if (!validFontDataHaiku(metadata, data.font))
                return std::nullopt;
            auto buffer = SharedBuffer::create(WTF::move(data.fontFaceData));
            auto custom = FontCustomPlatformData::create(buffer, data.itemInCollection);
            if (!custom)
                return std::nullopt;
            FontPlatformData result(metadata, custom.get());
            result.m_font = std::make_shared<BFont>(custom->m_font);
            if (!restoreFontHaiku(*result.m_font, metadata, data.font, true))
                return std::nullopt;
            return result;
        });
}

FontPlatformData::Attributes FontPlatformData::attributes() const
{
    return { m_metadata, serializeFontHaiku(font()) };
}

FontPlatformData FontPlatformData::create(const Attributes& attributes, const FontCustomPlatformData* custom)
{
    FontPlatformData result(attributes.m_metadata, custom);
    result.m_font = custom ? std::make_shared<BFont>(custom->m_font) : std::make_shared<BFont>();
    if (!restoreFontHaiku(*result.m_font, attributes.m_metadata, attributes.m_font, custom)) {
        // This API cannot return an error; retain a usable native fallback.
        result.m_font = std::make_shared<BFont>();
        result.m_metadata.pointSize = result.m_font->Size();
    }
    return result;
}

} // namespace WebCore
