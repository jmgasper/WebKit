/*
 * Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2025 Igalia S.L.
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
 */

#include "config.h"
#include "CoordinatedBackingStoreTile.h"

#if USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
#include "BitmapTexturePool.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsLayer.h"
#include <wtf/MallocSpan.h>
#include <wtf/SystemTracing.h>

#if USE(SKIA)
#include "BitmapTexture.h"
#include "SkiaPaintingEngine.h"
#endif

#if PLATFORM(HAIKU)
#include <OS.h>
#endif

namespace WebCore {

#if PLATFORM(HAIKU)
static thread_local bool s_countsUpdates;
static thread_local CoordinatedBackingStoreTile::UpdateStatsHaiku s_updateStats;

void CoordinatedBackingStoreTile::setCountsUpdatesHaiku(bool counts)
{
    s_countsUpdates = counts;
}

auto CoordinatedBackingStoreTile::takeUpdateStatsHaiku() -> UpdateStatsHaiku
{
    return std::exchange(s_updateStats, { });
}

FloatRect CoordinatedBackingStoreTile::rectAfterPendingUpdates() const
{
    if (m_updates.isEmpty())
        return m_rect;
    FloatRect rect(m_updates.last().tileRect);
    rect.scale(1. / m_scale);
    return rect;
}
#endif

CoordinatedBackingStoreTile::CoordinatedBackingStoreTile(float scale)
    : m_scale(scale)
{
}

CoordinatedBackingStoreTile::~CoordinatedBackingStoreTile() = default;

void CoordinatedBackingStoreTile::addUpdate(Update&& update)
{
    m_updates.append(WTF::move(update));
}

void CoordinatedBackingStoreTile::ensureTexture(const IntSize& size, CoordinatedTileBuffer& buffer)
{
    OptionSet<BitmapTexture::Flags> flags;
    if (buffer.supportsAlpha())
        flags.add(BitmapTexture::Flags::SupportsAlpha);

#if USE(SKIA) && USE(GBM)
    if (SkiaPaintingEngine::shouldUseLinearTileTextures()) {
        flags.add(BitmapTexture::Flags::BackedByDMABuf);
        flags.add(BitmapTexture::Flags::ForceLinearBuffer);
    } else if (SkiaPaintingEngine::shouldUseVivanteSuperTiledTileTextures()) {
        flags.add(BitmapTexture::Flags::BackedByDMABuf);
        flags.add(BitmapTexture::Flags::ForceVivanteSuperTiledBuffer);
    }
#endif

    WTFBeginSignpost(this, AcquireTexture);
    if (!m_texture)
        m_texture = BitmapTexturePool::singleton().acquireTexture(size, flags);
    else if (buffer.supportsAlpha() == m_texture->isOpaque())
        m_texture->reset(size, flags);
    WTFEndSignpost(this, AcquireTexture);
}

void CoordinatedBackingStoreTile::processPendingUpdates()
{
    auto updates = WTF::move(m_updates);
    auto updatesCount = updates.size();
    if (!updatesCount)
        return;

    WTFBeginSignpost(this, CoordinatedSwapBuffers, "%zu updates", updatesCount);
    for (unsigned updateIndex = 0; updateIndex < updatesCount; ++updateIndex) {
        auto& update = updates[updateIndex];

        WTFBeginSignpost(this, CoordinatedSwapBuffer, "%u/%zu, rect %ix%i+%i+%i", updateIndex + 1, updatesCount, update.tileRect.x(), update.tileRect.y(), update.tileRect.width(), update.tileRect.height());

#if PLATFORM(HAIKU)
        const bigtime_t waitStartedAt = s_countsUpdates ? system_time() : 0;
#endif
        update.buffer->waitUntilPaintingComplete();
#if PLATFORM(HAIKU)
        const bigtime_t paintedAt = s_countsUpdates ? system_time() : 0;
#endif

        FloatRect unscaledTileRect(update.tileRect);
        unscaledTileRect.scale(1. / m_scale);

        if (unscaledTileRect != m_rect) {
            m_rect = unscaledTileRect;
            m_texture = nullptr;
#if PLATFORM(HAIKU)
            m_isTransparent = false;
#endif
        }
#if PLATFORM(HAIKU)
        const bool hadTexture = !!m_texture;
#endif

#if USE(SKIA)
        if (update.buffer->isBackedByOpenGL()) {
            WTFBeginSignpost(this, CopyTextureGPUToGPU);
            auto& buffer = static_cast<CoordinatedAcceleratedTileBuffer&>(update.buffer.get());
            buffer.serverWait();

            Ref texture = buffer.texture();
            // Fast path: whole tile content changed -- take ownership of the incoming texture, replacing the existing tile buffer (avoiding texture copies).
            if (update.sourceRect.size() == update.tileRect.size()) {
                ASSERT(update.sourceRect.location().isZero());
                if (m_texture)
                    m_texture->swapTexture(texture);
                else
                    m_texture = WTF::move(texture);
            } else {
                ensureTexture(update.tileRect.size(), buffer);
                m_texture->copyFromExternalTexture(texture->id(), update.sourceRect, toIntSize(update.bufferOffset));
            }

            WTFEndSignpost(this, CopyTextureGPUToGPU);
            WTFEndSignpost(this, CoordinatedSwapBuffer);
            continue;
        }
#endif
        auto& buffer = static_cast<CoordinatedUnacceleratedTileBuffer&>(update.buffer.get());
#if PLATFORM(HAIKU)
        const bool coversTile = update.sourceRect.location().isZero() && update.sourceRect.size() == update.tileRect.size();
        if (buffer.isTransparent() && (coversTile || m_isTransparent)) {
            // Nothing to show: no texture, and nothing drawn for the tile.
            m_texture = nullptr;
            m_isTransparent = true;
            if (s_countsUpdates)
                ++s_updateStats.transparentTiles;
            WTFEndSignpost(this, CoordinatedSwapBuffer);
            continue;
        }
        if (m_isTransparent && !coversTile) {
            // Part of a tile that had nothing is painted: the rest of its
            // new texture, which may have been another tile's, is cleared.
            ensureTexture(update.tileRect.size(), buffer);
            auto nothing = MallocSpan<unsigned char>::zeroedMalloc(static_cast<size_t>(update.tileRect.width()) * update.tileRect.height() * 4);
            m_texture->updateContents(nothing.span().data(), IntRect({ }, update.tileRect.size()), { }, update.tileRect.width() * 4, update.buffer->pixelFormat());
        }
        m_isTransparent = false;
#endif
        ensureTexture(update.tileRect.size(), buffer);
#if PLATFORM(HAIKU)
        const bigtime_t texturedAt = s_countsUpdates ? system_time() : 0;
#endif

        WTFBeginSignpost(this, CopyTextureCPUToGPU);
        ASSERT(!update.buffer->isBackedByOpenGL());
        m_texture->updateContents(buffer.data(), update.sourceRect, update.bufferOffset, buffer.stride(), update.buffer->pixelFormat());
        WTFEndSignpost(this, CopyTextureCPUToGPU);
#if PLATFORM(HAIKU)
        if (s_countsUpdates) {
            ++s_updateStats.tiles;
            if (!hadTexture)
                ++s_updateStats.newTextures;
            s_updateStats.megapixels += update.sourceRect.width() * double(update.sourceRect.height()) / 1e6;
            s_updateStats.paintWaitMs += (paintedAt - waitStartedAt) / 1000.0;
            s_updateStats.textureMs += (texturedAt - paintedAt) / 1000.0;
            s_updateStats.uploadMs += (system_time() - texturedAt) / 1000.0;
        }
#endif

        WTFEndSignpost(this, CoordinatedSwapBuffer);
    }
    WTFEndSignpost(this, CoordinatedSwapBuffers);
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(TEXTURE_MAPPER)
