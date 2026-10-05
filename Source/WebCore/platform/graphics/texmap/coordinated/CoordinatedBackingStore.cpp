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

#include "config.h"
#include "CoordinatedBackingStore.h"

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
#include "BitmapTexture.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsLayer.h"
#include "TextureMapper.h"
#include "TextureMapperFlags.h"
#include <wtf/SystemTracing.h>

#if PLATFORM(HAIKU)
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <wtf/Scope.h>
#endif

namespace WebCore {

void CoordinatedBackingStore::createTile(uint32_t id)
{
    m_tiles.add(id, CoordinatedBackingStoreTile(m_scale));
}

void CoordinatedBackingStore::removeTile(uint32_t id)
{
    ASSERT(m_tiles.contains(id));
    m_tiles.remove(id);
}

void CoordinatedBackingStore::updateTile(uint32_t id, const IntRect& sourceRect, const IntRect& tileRect, Ref<CoordinatedTileBuffer>&& buffer, const IntPoint& offset)
{
    auto it = m_tiles.find(id);
    ASSERT(it != m_tiles.end());
    it->value.addUpdate({ WTF::move(buffer), sourceRect, tileRect, offset });
    m_hasPendingTileUpdates = true;
}

#if PLATFORM(HAIKU)
static thread_local int64_t s_uploadDeadline;
static thread_local bool s_hasDeferredUploads;
static thread_local bool s_showsLateUpload;
static thread_local IntRect s_viewport;

void CoordinatedBackingStore::setUploadDeadlineHaiku(int64_t systemTime)
{
    s_uploadDeadline = systemTime;
}

void CoordinatedBackingStore::setViewportHaiku(const IntRect& viewport)
{
    s_viewport = viewport;
}

bool CoordinatedBackingStore::takeHasDeferredUploadsHaiku()
{
    return std::exchange(s_hasDeferredUploads, false);
}

bool CoordinatedBackingStore::takeShowsLateUploadHaiku()
{
    return std::exchange(s_showsLateUpload, false);
}

// Tiles a composition had to leave out where it draws: "waiting" ones have
// an update that was not uploaded, the others have no texture (nothing
// painted in them, or nothing received yet). SUMMIT_TILE_HOLE_TRACE=1 names
// the layer's store for each composition that leaves a waiting tile out.
static thread_local unsigned s_waitingTilesInClip;
static thread_local unsigned s_texturelessTilesInClip;

unsigned summitTakeTileHolesHaiku(unsigned& textureless)
{
    textureless = std::exchange(s_texturelessTilesInClip, 0);
    return std::exchange(s_waitingTilesInClip, 0);
}
#endif

void CoordinatedBackingStore::processPendingUpdates()
{
    if (!m_hasPendingTileUpdates)
        return;

#if PLATFORM(HAIKU)
    // Uploading a tile costs about 50 us, and one rendering update of a page
    // that is being scrolled brings 60 to 600 of them: 3 to 90 ms in a
    // composition that has 16. Most of those tiles are outside the viewport,
    // painted ahead of the scroll or repainted because the page keeps them.
    // What the last composition showed of this layer, and a margin for the
    // distance a scroll covers until the next, is uploaded now.
    if (s_uploadDeadline && m_hasLastPaint) {
        static constexpr int margin = 256;
        // The viewport, not the last composition's clip: with damage
        // tracking that clip is often only what changed, and tiles in view
        // were taken for tiles outside it. A composition that had used its
        // budget before it reached such a layer left them again, so parts of
        // GitHub's issue list kept showing an earlier layout.
        FloatRect shown = s_viewport.isEmpty() ? FloatRect(m_lastPaintClip) : FloatRect(s_viewport);
        shown.inflate(margin);
        bool deferred = false;
        Vector<CoordinatedBackingStoreTile*, 64> others;
        for (auto& tile : m_tiles.values()) {
            if (!tile.hasPendingUpdates())
                continue;
            if (!m_lastPaintTransform.isAffine() || m_lastPaintTransform.mapRect(tile.rectAfterPendingUpdates()).intersects(shown)) {
                tile.processPendingUpdates();
                if (m_hasDeferredTiles)
                    s_showsLateUpload = true;
            } else
                others.append(&tile);
        }
        for (auto* tile : others) {
            if (system_time() >= s_uploadDeadline) {
                deferred = true;
                break;
            }
            tile->processPendingUpdates();
        }
        m_hasPendingTileUpdates = deferred;
        m_hasDeferredTiles = deferred;
        if (deferred)
            s_hasDeferredUploads = true;
        return;
    }
    if (m_hasDeferredTiles)
        s_showsLateUpload = true;
    m_hasDeferredTiles = false;
#endif

    for (auto& tile : m_tiles.values())
        tile.processPendingUpdates();

    m_hasPendingTileUpdates = false;
}

void CoordinatedBackingStore::resize(const FloatSize& size, float scale)
{
    m_size = size;
    m_scale = scale;
}

void CoordinatedBackingStore::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& transform, float opacity)
{
    if (m_tiles.isEmpty())
        return;

    ASSERT(!m_size.isZero());
    FloatRect layerRect = { { }, m_size };
    TransformationMatrix adjustedTransform = transform * TransformationMatrix::rectToRect(layerRect, targetRect);
#if PLATFORM(HAIKU)
    const FloatRect clip = textureMapper.clipBounds();
    // A composition that draws only what changed clips to that; what the
    // layer shows is the widest clip it has been drawn with at this place.
    if (!m_hasLastPaint || m_lastPaintTransform != adjustedTransform || !m_lastPaintClip.contains(enclosingIntRect(clip)))
        m_lastPaintClip = enclosingIntRect(clip);
    m_lastPaintTransform = adjustedTransform;
    m_hasLastPaint = true;
    // Where a perspective transform puts a rectangle is not a rectangle.
    const bool cullsTiles = adjustedTransform.isAffine();
#endif

#if ENABLE(DAMAGE_TRACKING)
    const auto& frameDamage = textureMapper.damage();
    const auto canUseDamageToDrawTextureFragment = [&]() {
        return frameDamage
            && !frameDamage->isEmpty()
            && frameDamage->mode() != Damage::Mode::Full
            && adjustedTransform.isIdentity()
            && opacity == 1.0;
    }();
#endif

#if PLATFORM(HAIKU)
    unsigned waitingTiles = 0;
    auto reportHoles = makeScopeExit([&] {
        static const bool traceHoles = [] {
            const char* value = getenv("SUMMIT_TILE_HOLE_TRACE");
            return value && value[0] == '1' && !value[1];
        }();
        if (traceHoles && waitingTiles)
            fprintf(stderr, "Summit tile hole: at=%lld store=%p size=%.0fx%.0f scale=%.2f tiles=%u waiting=%u origin=%.0f,%.0f clip=%.0f,%.0f %.0fx%.0f\n",
                static_cast<long long>(system_time()), static_cast<void*>(this), m_size.width(), m_size.height(), m_scale, m_tiles.size(), waitingTiles,
                adjustedTransform.m41(), adjustedTransform.m42(), clip.x(), clip.y(), clip.width(), clip.height());
    });
#endif
    for (const auto& tile : m_tiles.values()) {
        ASSERT(tile.scale() == m_scale);
#if PLATFORM(HAIKU)
        // A tile whose first upload was left for a later composition.
        if (!tile.canBePainted()) {
            if (!cullsTiles || adjustedTransform.mapRect(tile.hasPendingUpdates() ? tile.rectAfterPendingUpdates() : tile.rect()).intersects(clip)) {
                if (tile.hasPendingUpdates()) {
                    ++s_waitingTilesInClip;
                    ++waitingTiles;
                } else
                    ++s_texturelessTilesInClip;
            }
            continue;
        }
        // A layer keeps tiles for twice the viewport and more; each costs
        // a draw call (25 us in the driver) whether it shows or not. The
        // clip is what this composition draws: the viewport, or the part of
        // it that changed.
        if (cullsTiles && !adjustedTransform.mapRect(tile.rect()).intersects(clip))
            continue;
#endif
        const auto allEdgesExposed = allTileEdgesExposed(layerRect, tile.rect()) ? TextureMapper::AllEdgesExposed::Yes : TextureMapper::AllEdgesExposed::No;

#if ENABLE(DAMAGE_TRACKING)
        if (canUseDamageToDrawTextureFragment && allEdgesExposed == TextureMapper::AllEdgesExposed::No && tile.texture().isOpaque() && !tile.texture().filterOperation()) {
            const auto tileDamageRect = intersection(tile.rect(), frameDamage->bounds());
            if (!tileDamageRect.isEmpty()) {
                const auto sourceRect = FloatRect { FloatPoint { tileDamageRect.location() - tile.rect().location() }, tileDamageRect.size() };
                textureMapper.drawTextureFragment(tile.texture(), sourceRect, tileDamageRect);
            }

            continue;
        }
#endif
        textureMapper.drawTexture(tile.texture(), tile.rect(), adjustedTransform, opacity, allEdgesExposed);
    }
}

void CoordinatedBackingStore::drawBorder(TextureMapper& textureMapper, const Color& borderColor, float borderWidth, const FloatRect& targetRect, const TransformationMatrix& transform)
{
    ASSERT(!m_size.isZero());
    FloatRect layerRect = { { }, m_size };
    TransformationMatrix adjustedTransform = transform * TransformationMatrix::rectToRect(layerRect, targetRect);
    for (const auto& tile : m_tiles.values())
        textureMapper.drawBorder(borderColor, borderWidth, tile.rect(), adjustedTransform);
}

void CoordinatedBackingStore::drawRepaintCounter(TextureMapper& textureMapper, int repaintCount, const Color& borderColor, const FloatRect& targetRect, const TransformationMatrix& transform)
{
    ASSERT(!m_size.isZero());
    FloatRect layerRect = { { }, m_size };
    TransformationMatrix adjustedTransform = transform * TransformationMatrix::rectToRect(layerRect, targetRect);
    for (const auto& tile : m_tiles.values())
        textureMapper.drawNumber(repaintCount, borderColor, tile.rect().location(), adjustedTransform);
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
