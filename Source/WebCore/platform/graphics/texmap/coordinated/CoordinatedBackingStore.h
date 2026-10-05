/*
 Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies)

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
#include "CoordinatedBackingStoreTile.h"
#include "TextureMapperBackingStore.h"
#include "IntRect.h"
#include "TransformationMatrix.h"
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>

namespace WebCore {
class CoordinatedTileBuffer;
class TextureMapper;

class CoordinatedBackingStore final : public RefCounted<CoordinatedBackingStore>, public TextureMapperBackingStore {
public:
    static Ref<CoordinatedBackingStore> create()
    {
        return adoptRef(*new CoordinatedBackingStore);
    }
    ~CoordinatedBackingStore() = default;

    void resize(const FloatSize&, float scale);
    float scale() const { return m_scale; }

    void createTile(uint32_t tileID);
    void removeTile(uint32_t tileID);
    void updateTile(uint32_t tileID, const IntRect&, const IntRect&, Ref<CoordinatedTileBuffer>&&, const IntPoint&);

    bool hasPendingUpdates() const { return m_hasPendingTileUpdates; }
    void processPendingUpdates();

#if PLATFORM(HAIKU)
    // A composition uploads the tiles it is about to show, and of the others
    // (a scroll paints tiles ahead of the viewport, a repaint covers the
    // tiles kept around it) as many as fit before this time; the rest wait
    // for the next composition. Zero uploads everything at once. Set and
    // read on the compositor thread.
    static void setUploadDeadlineHaiku(int64_t systemTime);
    // The area the composition shows, in the coordinates layers are painted
    // in; tiles that fall in it are uploaded whatever the deadline.
    static void setViewportHaiku(const IntRect&);
    // Whether a backing store has left tiles for the next composition since
    // this was last asked.
    static bool takeHasDeferredUploadsHaiku();
    // Whether a tile that had waited is uploaded into view by this
    // composition: the damage of its update went with the composition that
    // received it, so this one has to present the whole frame.
    static bool takeShowsLateUploadHaiku();
#endif

    void paintToTextureMapper(TextureMapper&, const FloatRect&, const TransformationMatrix&, float) override;
    void drawBorder(TextureMapper&, const Color&, float borderWidth, const FloatRect&, const TransformationMatrix&) override;
    void drawRepaintCounter(TextureMapper&, int repaintCount, const Color&, const FloatRect&, const TransformationMatrix&) override;

private:
    CoordinatedBackingStore() = default;

    HashMap<uint32_t, CoordinatedBackingStoreTile> m_tiles;
    FloatSize m_size;
    float m_scale { 1. };
    bool m_hasPendingTileUpdates { false };
#if PLATFORM(HAIKU)
    // Where the last composition drew this layer, to tell which tiles show.
    TransformationMatrix m_lastPaintTransform;
    IntRect m_lastPaintClip;
    bool m_hasLastPaint { false };
    bool m_hasDeferredTiles { false };
#endif
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
