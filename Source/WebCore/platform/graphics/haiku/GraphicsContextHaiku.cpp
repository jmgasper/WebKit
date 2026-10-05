/*
 * Copyright (C) 2007 Ryan Leavengood <leavengood@gmail.com>
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 * Copyright (C) 2015 Julian Harnath <julian.harnath@rwth-aachen.de>
 *
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <algorithm>
#include <cmath>
#include "GraphicsContextHaiku.h"

#include "AffineTransform.h"
#include "Color.h"
#include "DisplayListRecorder.h"
#include "Gradient.h"
#include "GraphicsContextStateSaver.h"
#include "ImageBuffer.h"
#include "NotImplemented.h"
#include "NativeImage.h"
#include "Path.h"
#include "Pattern.h"
#include "TransformationMatrix.h"
#include "ShadowBlur.h"

#include <wtf/text/CString.h>
#include "PaintStatisticsHaiku.h"
#include <wtf/text/TextStream.h>
#include <Bitmap.h>
#include <GraphicsDefs.h>
#include <Picture.h>
#include <Region.h>
#include <Shape.h>
#include <Window.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//#define TRACE_GRAPHICS_HAIKU
#ifdef TRACE_GRAPHICS_HAIKU
#	define HGTRACE(x) printf x
#else
#	define HGTRACE(x) ;
#endif

namespace {

class BlendModeGuard {
    BView* m_view;
    source_alpha m_sa;
    alpha_function m_af;
public:
    BlendModeGuard(BView* view): m_view(view) {
        m_view->GetBlendingMode(&m_sa, &m_af);
    }
    
    ~BlendModeGuard() {
        m_view->SetBlendingMode(m_sa, m_af);
    }
};

}

namespace WebCore {

// app_server answers HighColor(), PenSize(), DrawingMode() and FillRule() with a
// synchronous round trip, and PopState() invalidates the client's copy, so a
// page that saves and restores state constantly asked over and over for values
// this port set itself. These four restore from the state instead.
static drawing_mode drawingModeForCompositeHaiku(CompositeOperator operation)
{
    // Mirrors what didUpdateState() sends for Change::CompositeMode.
    switch (operation) {
    case CompositeOperator::Copy:
        return B_OP_COPY;
    default:
        return B_OP_ALPHA;
    }
}


// The context's global alpha multiplies every color it draws with.
static rgb_color withGlobalAlpha(rgb_color color, float alpha)
{
    if (alpha < 1)
        color.alpha = static_cast<uint8_t>(std::lround(color.alpha * std::clamp(alpha, 0.0f, 1.0f)));
    return color;
}

GraphicsContextHaiku::GraphicsContextHaiku(BView* view)
    : GraphicsContext(IsDeferred::No, {
        GraphicsContextState::Change::StrokeThickness,
        GraphicsContextState::Change::StrokeBrush,
        GraphicsContextState::Change::Alpha,
        GraphicsContextState::Change::StrokeStyle,
        GraphicsContextState::Change::FillBrush,
        GraphicsContextState::Change::FillRule,
        GraphicsContextState::Change::CompositeMode,
    })
    , m_view(view)
    , m_strokeStyle(B_SOLID_HIGH)
    , m_painter(nullptr)
{
    HGTRACE(("============= GraphicsContextHaiku new %p =============\n", view));
    didUpdateState(m_state);
    
    m_fillBitmap = new BBitmap(BRect(0, 0, 5, 5), B_RGBA32);
    memset(m_fillBitmap->Bits(), 0, m_fillBitmap->BitsLength());
    
    m_view->SetDrawingMode(B_OP_ALPHA);
    m_view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
}

GraphicsContextHaiku::~GraphicsContextHaiku()
{
    HGTRACE(("============= GraphicsContextHaiku destroy %p =============\n", m_view));
    delete m_fillBitmap;
}

// Draws a filled rectangle with a stroked border.
void GraphicsContextHaiku::drawRect(const FloatRect& rect, float borderThickness)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("drawRect: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    if (RefPtr pattern = m_state.fillBrush().pattern()) {
        GraphicsContextStateSaver stateSaver(*this);
        clip(rect);
        fillClipWithPattern(*pattern, rect);
    } else if (m_state.fillBrush().gradient()) {
        m_state.fillBrush().gradient()->fill(*this, rect);
    } else
        m_view->FillRect(rect, B_SOLID_LOW);

    // TODO: Support gradients
    strokeRect(rect, borderThickness);
}

void GraphicsContextHaiku::drawNativeImage(const NativeImage& image, const FloatRect& destRect, const FloatRect& srcRect, ImagePaintingOptions options)
{
    PaintTimerHaiku imageTimer(paintStatisticsHaiku().imageTime);
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.images;
    HGTRACE(("drawNativeImage:  src([%f:%f] [%f:%f])\n", srcRect.x(), srcRect.y(), srcRect.width(), srcRect.height()));
    HGTRACE(("                 dest([%f:%f] [%f:%f])\n", destRect.x(), destRect.y(), destRect.width(), destRect.height()));
    drawBitmap(image.platformImage().get(), destRect, srcRect, options);
}

void GraphicsContextHaiku::drawBitmap(BBitmap* image, const FloatRect& destRect, const FloatRect& srcRect, const ImagePaintingOptions& options)
{
    HGTRACE(("drawBitmap:  src([%f:%f] [%f:%f])\n", srcRect.x(), srcRect.y(), srcRect.width(), srcRect.height()));
    HGTRACE(("            dest([%f:%f] [%f:%f])\n", destRect.x(), destRect.y(), destRect.width(), destRect.height()));

    GraphicsContextStateSaver stateSaver(*this);
    setCompositeOperation(options.compositeOperator());

    // Test using example site at
    // http://www.meyerweb.com/eric/css/edge/complexspiral/demo.html
    //m_view->SetDrawingMode(B_OP_ALPHA);

    uint32 flags = 0;

    // TODO handle more things from options (blend mode, etc)
    if (options.interpolationQuality() > InterpolationQuality::Low)
        flags |= B_FILTER_BITMAP_BILINEAR;

    // FIXME: Async doesn't draw shadows (rarely they do appear,
    // so there is a data race somewhere)
    m_view->DrawBitmap(image, BRect(srcRect), BRect(destRect), flags);
}

// This is only used to draw borders.
// The line width is already accounted for, the points being not the center of
// the edges, but opposite corners of the rectangle containing the line.
void GraphicsContextHaiku::drawLine(const FloatPoint& point1, const FloatPoint& point2)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("drawline: [%f:%f] [%f:%f])\n", point1.x(), point1.y(), point2.x(), point2.y()));
    if (strokeStyle() == WebCore::StrokeStyle::NoStroke || !strokeColor().isVisible())
        return;

    BPoint start = point1;
    BPoint end = point2;
    // This test breaks for a vertical line as wide as long, but in that
    // case there's no information to tell vertical and horizontal apart.
    if (fabs(end.y - start.y - strokeThickness()) < 1) {
        // Horizontal line
        end.y = start.y = (end.y + start.y) / 2;
        end.x--;
    } else {
        // Vertical line
        end.x = start.x = (end.x + start.x) / 2;
        end.y--;
    }
    m_view->StrokeLine(start, end, m_strokeStyle);
}

// This method is only used to draw the little circles used in lists.
void GraphicsContextHaiku::drawEllipse(const FloatRect& rect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("drawEllipse: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    if (m_state.fillBrush().pattern() || m_state.fillBrush().gradient() || fillColor().isVisible()) {
//        TODO: What's this shadow business?
        if (m_state.fillBrush().pattern())
            notImplemented();
        else if (m_state.fillBrush().gradient()) {
            const BGradient& gradient = m_state.fillBrush().gradient()->getHaikuGradient();
            m_view->FillEllipse(rect, gradient);
        } else
            m_view->FillEllipse(rect, B_SOLID_LOW);
    }

    // TODO: Support gradients
    if (strokeStyle() != WebCore::StrokeStyle::NoStroke && strokeThickness() > 0.0f && strokeColor().isVisible())
        m_view->StrokeEllipse(rect, m_strokeStyle);
}

void GraphicsContextHaiku::strokeRect(const FloatRect& rect, float width)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("strokeRect: [%f:%f] [%f:%f] width:%f\n", rect.x(), rect.y(), rect.width(), rect.height(), width));
    if (strokeStyle() == WebCore::StrokeStyle::NoStroke || width <= 0.0f || !strokeColor().isVisible())
        return;

    m_view->SetPenSize(width);
    // TODO stroke the shadow
    m_view->StrokeRect(rect, m_strokeStyle);
    m_view->SetPenSize(strokeThickness());
}

void GraphicsContextHaiku::strokePath(const Path& path)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("strokePath: (--todo print values)\n"));
    m_view->MovePenTo(B_ORIGIN);

    // TODO: stroke the shadow (cf shadowAndStrokeCurrentCairoPath)

    if (m_state.strokeBrush().pattern())
        notImplemented();
    else if (m_state.strokeBrush().gradient()) {
        notImplemented();
//      BGradient* gradient = m_state.strokeGradient->platformGradient();
//      m_view->StrokeShape(shape(), *gradient);
    } else if (strokeColor().isVisible()) {
        m_view->StrokeShape(path.platformPath(), m_strokeStyle);
    }
}

void GraphicsContextHaiku::fillRect(const FloatRect& rect, const Color& color)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("fillRect(color): [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));

    if (hasDropShadow()) {
        HGTRACE(("hasDropShadow begin\n"));
        const auto shadow = dropShadow();
        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, FloatRoundedRect(rect));
        HGTRACE(("hasDropShadow end\n"));
    }
    
    // FillRect doesn't respect blending modes, DrawBitmap does
    const auto [r, g, b, colorAlpha] = color.toColorTypeLossy<SRGBA<uint8_t>>().resolved();
    const uint32_t a = withGlobalAlpha(make_color(r, g, b, colorAlpha), alpha()).alpha;
    const uint32_t c = ((a << 24) | (r << 16) | (g << 8) | b);
    m_fillBitmap->Lock();
    uint32_t *bits = reinterpret_cast<uint32_t *>(m_fillBitmap->Bits());
    if(bits[0] != c) {
        std::fill(bits, bits + m_fillBitmap->BitsLength() / 4, c);
    }
    // cannot be async because bitmap might change before the draw is executed
    m_view->DrawTiledBitmap(m_fillBitmap, BRect(rect));
    m_fillBitmap->Unlock();
}

void GraphicsContextHaiku::fillRect(const FloatRect& rect, RequiresClipToRect requiresClipToRect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("fillRect: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    if (RefPtr pattern = fillPattern()) {
        GraphicsContextStateSaver stateSaver(*this);
        clip(rect);
        fillClipWithPattern(*pattern, rect);
        return;
    }
    if (RefPtr fillGradient = this->fillGradient()) {
        fillRect(rect, *fillGradient, fillGradientSpaceTransform(), requiresClipToRect);
        return;
    }    
    
    if (hasDropShadow()) {
        HGTRACE(("hasDropShadow begin\n"));
        const auto shadow = dropShadow();
        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, FloatRoundedRect(rect));
        HGTRACE(("hasDropShadow end\n"));
    }
    // FillRect doesn't respect blending modes, DrawBitmap does
    const auto [r, g, b, colorAlpha] = state().fillBrush().color().toColorTypeLossy<SRGBA<uint8_t>>().resolved();
    const uint32_t a = withGlobalAlpha(make_color(r, g, b, colorAlpha), alpha()).alpha;
    const uint32_t c = ((a << 24) | (r << 16) | (g << 8) | b);
    m_fillBitmap->Lock();
    uint32_t *bits = reinterpret_cast<uint32_t *>(m_fillBitmap->Bits());
    if(bits[0] != c) {
        std::fill(bits, bits + m_fillBitmap->BitsLength() / 4, c);
    }
    // cannot be async because bitmap might change before the draw is executed
    m_view->DrawTiledBitmap(m_fillBitmap, BRect(rect));
    m_fillBitmap->Unlock();
}

void GraphicsContextHaiku::fillRect(const WebCore::FloatRect& r, WebCore::Gradient& g, const WebCore::AffineTransform&, RequiresClipToRect requiresClipToRect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("fillRect(gradient): [%f:%f] [%f:%f]\n", r.x(), r.y(), r.width(), r.height()));
    if (requiresClipToRect == RequiresClipToRect::Yes) {
        m_view->ClipToRect(r);
    }
    
    // TODO handle the transform
    m_view->FillRect(r, g.getHaikuGradient());
}

// Paints the pattern over bounds, which the caller has already clipped to the filled shape.
void GraphicsContextHaiku::fillClipWithPattern(Pattern& pattern, const FloatRect& bounds)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    RefPtr image = pattern.tileNativeImage();
    BBitmap* bitmap = image ? image->platformImage().get() : nullptr;
    auto inverse = pattern.patternSpaceTransform().inverse();
    if (!bitmap || !bitmap->IsValid() || !inverse || bounds.isEmpty())
        return;
    FloatSize tile = image->size();
    if (tile.isEmpty())
        return;
    // Draw in pattern space, where the tile's origin is (0, 0).
    concatCTM(pattern.patternSpaceTransform());
    FloatRect area = inverse->mapRect(bounds);
    if (!pattern.repeatX()) {
        float left = std::max(area.x(), 0.0f);
        area.setWidth(std::min(area.maxX(), tile.width()) - left);
        area.setX(left);
    }
    if (!pattern.repeatY()) {
        float top = std::max(area.y(), 0.0f);
        area.setHeight(std::min(area.maxY(), tile.height()) - top);
        area.setY(top);
    }
    if (area.isEmpty())
        return;
    float phaseX = std::fmod(area.x(), tile.width());
    float phaseY = std::fmod(area.y(), tile.height());
    if (phaseX < 0)
        phaseX += tile.width();
    if (phaseY < 0)
        phaseY += tile.height();
    m_view->PushState();
    m_view->SetDrawingMode(B_OP_ALPHA);
    m_view->ClipToRect(BRect(area));
    // Same phase convention as CSS background tiling in drawBitmap(): the distance from
    // the tile origin to the drawn rectangle's corner, modulo the tile size.
    m_view->DrawTiledBitmap(bitmap, BRect(area), BPoint(phaseX, phaseY));
    m_view->PopState();
}

void GraphicsContextHaiku::fillRoundedRectImpl(const FloatRoundedRect& roundRect, const Color& color)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("fillRoundedRectImpl: [%f:%f] [%f:%f]\n", roundRect.rect().x(), roundRect.rect().y(), roundRect.rect().width(), roundRect.rect().height()));
    if (!color.isVisible())
        return;

    const FloatRect& rect = roundRect.rect();
    const FloatSize& topLeft = roundRect.radii().topLeft();
    const FloatSize& topRight = roundRect.radii().topRight();
    const FloatSize& bottomLeft = roundRect.radii().bottomLeft();
    const FloatSize& bottomRight = roundRect.radii().bottomRight();

    if (hasDropShadow()) {
        HGTRACE(("hasDropShadow begin\n"));
        const auto shadow = dropShadow();
        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, roundRect);
        HGTRACE(("hasDropShadow end\n"));
    }

    BPoint points[3];
    const float kRadiusBezierScale = 1.0f - 0.5522847498f; //  1 - (sqrt(2) - 1) * 4 / 3

    BShape shape;
    shape.MoveTo(BPoint(rect.maxX() - topRight.width(), rect.y()));
    points[0].x = rect.maxX() - kRadiusBezierScale * topRight.width();
    points[0].y = rect.y();
    points[1].x = rect.maxX();
    points[1].y = rect.y() + kRadiusBezierScale * topRight.height();
    points[2].x = rect.maxX();
    points[2].y = rect.y() + topRight.height();
    shape.BezierTo(points);
    shape.LineTo(BPoint(rect.maxX(), rect.maxY() - bottomRight.height()));
    points[0].x = rect.maxX();
    points[0].y = rect.maxY() - kRadiusBezierScale * bottomRight.height();
    points[1].x = rect.maxX() - kRadiusBezierScale * bottomRight.width();
    points[1].y = rect.maxY();
    points[2].x = rect.maxX() - bottomRight.width();
    points[2].y = rect.maxY();
    shape.BezierTo(points);
    shape.LineTo(BPoint(rect.x() + bottomLeft.width(), rect.maxY()));
    points[0].x = rect.x() + kRadiusBezierScale * bottomLeft.width();
    points[0].y = rect.maxY();
    points[1].x = rect.x();
    points[1].y = rect.maxY() - kRadiusBezierScale * bottomLeft.height();
    points[2].x = rect.x();
    points[2].y = rect.maxY() - bottomLeft.height();
    shape.BezierTo(points);
    shape.LineTo(BPoint(rect.x(), rect.y() + topLeft.height()));
    points[0].x = rect.x();
    points[0].y = rect.y() + kRadiusBezierScale * topLeft.height();
    points[1].x = rect.x() + kRadiusBezierScale * topLeft.width();
    points[1].y = rect.y();
    points[2].x = rect.x() + topLeft.width();
    points[2].y = rect.y();
    shape.BezierTo(points);
    shape.Close(); // Automatically completes the shape with the top border

    m_view->MovePenTo(B_ORIGIN);
    // The high color is the stroke color; put it back so later strokes are not
    // affected. didUpdateState() derives it from the state, so this does too.
    m_view->SetHighColor(withGlobalAlpha(color, alpha()));
    m_view->FillShape(&shape);
    m_view->SetHighColor(withGlobalAlpha(m_state.strokeBrush().color(), alpha()));
}

void GraphicsContextHaiku::fillRectWithRoundedHole(const FloatRect& rect, const FloatRoundedRect& roundedHoleRect, const Color& color)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("fillRectWithRoundedHole: [%f:%f] [%f:%f] hole: [%f:%f] [%f:%f]\n",
        rect.x(), rect.y(), rect.width(), rect.height(),
        roundedHoleRect.rect().x(), roundedHoleRect.rect().y(), roundedHoleRect.rect().width(), roundedHoleRect.rect().height()));
    Path path;
    path.addRect(rect);

    if (!roundedHoleRect.radii().isZero())
        path.addRoundedRect(roundedHoleRect);
    else
        path.addRect(roundedHoleRect.rect());

    WindRule oldFillRule = fillRule();
    Color oldFillColor = fillColor();

    setFillRule(WindRule::EvenOdd);
    setFillColor(color);

    if (hasDropShadow()) {
        HGTRACE(("hasDropShadow begin\n"));
        const auto shadow = dropShadow();
        ASSERT(shadow);
        
        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawInsetShadow(*this, rect, roundedHoleRect);
        HGTRACE(("hasDropShadow end\n"));
    }

    fillPath(path);

    setFillRule(oldFillRule);
    setFillColor(oldFillColor);
}

void GraphicsContextHaiku::fillPath(const Path& path)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
#if 0
    TextStream ts;
    ts << "fillPath " << path;
    HGTRACE(("%s\n", ts.release().ascii().data()));
    FloatRect rect = path.fastBoundingRect();
    FloatSize layerSize = getCTM().mapSize(rect.size());
    FloatRect mappedRect = getCTM().mapRect(rect);
    HGTRACE(("fillPath: fastBoundingRect = [%f:%f] [%f:%f], mappedRect = [%f %f]\n",
        rect.x(), rect.y(), rect.width(), rect.height(), mappedRect.x(), mappedRect.y()));
    BRect pathBounds = BRect(0, 0, rect.width() + 1, rect.height() + 1);
    BBitmap *pathBitmap = new BBitmap(pathBounds, B_RGBA32, true, false);
    if(m_painter == nullptr) {
        m_painter = new BView(pathBounds, "painter", B_FOLLOW_ALL, B_WILL_DRAW);
    }
    if(!m_painter->Bounds().Contains(pathBounds)) {
        m_painter->ResizeTo(pathBounds.Size());
    }
    pathBitmap->AddChild(m_painter);
    pathBitmap->Lock();
    m_painter->PushState();
    m_painter->SetFillRule(fillRule() == WindRule::NonZero ? B_NONZERO : B_EVEN_ODD);
    m_painter->MovePenTo(B_ORIGIN);
    //m_painter->TranslateBy(-rect.x(), -rect.y());
    m_painter->SetDrawingMode(B_OP_ALPHA);
    m_painter->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    
    // TODO: renderShadow

    if (m_state.fillBrush().pattern()) {
        HGTRACE(("fillPath(pattern)\n"));
        notImplemented();
    } else if (m_state.fillBrush().gradient()) {
        HGTRACE(("fillPath(gradient)\n"));
        const BGradient& gradient = m_state.fillBrush().gradient()->getHaikuGradient();
        m_painter->FillShape(path.platformPath(), gradient);
    } else {
        HGTRACE(("fillPath(else)\n"));
        m_painter->SetHighColor(m_state.fillBrush().color());
        m_painter->FillShape(path.platformPath(), B_SOLID_HIGH);
    }
    m_painter->PopState();
    pathBitmap->RemoveChild(m_painter);
    
    BlendModeGuard guard(m_view);
    m_view->SetDrawingMode(B_OP_ALPHA);
    m_view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    getCTM();
    m_view->PushState();
    // FIXME: rect has negative coordinates so it writes all over memory
    m_view->ClipToRect(m_view->Frame());
    //m_view->DrawBitmap(pathBitmap, B_ORIGIN);
    m_view->PopState();
    pathBitmap->Unlock();
    delete pathBitmap;
#endif
    HGTRACE(("fillPath: (--todo print values)\n"));
    m_view->SetFillRule(fillRule() == WindRule::NonZero ? B_NONZERO : B_EVEN_ODD);
    m_view->MovePenTo(B_ORIGIN);

    // TODO: Render the shadow (cf shadowAndFillCurrentCairoPath)
    drawing_mode mode = drawingModeForCompositeHaiku(compositeOperation());

    if (RefPtr pattern = m_state.fillBrush().pattern()) {
        GraphicsContextStateSaver stateSaver(*this);
        clipPath(path, fillRule());
        fillClipWithPattern(*pattern, path.fastBoundingRect());
        return;
    } else if (m_state.fillBrush().gradient()) {
        m_view->SetDrawingMode(B_OP_ALPHA);
        const BGradient& gradient = m_state.fillBrush().gradient()->getHaikuGradient();
        m_view->FillShape(path.platformPath(), gradient);
    } else {
        // The shape is filled with the low colour, which is the fill brush.
        if (withGlobalAlpha(m_state.fillBrush().color(), alpha()).alpha < 255)
            m_view->SetDrawingMode(B_OP_ALPHA);

        m_view->FillShape(path.platformPath(), B_SOLID_LOW);
    }

    m_view->SetDrawingMode(mode);
}

void GraphicsContextHaiku::clip(const FloatRect& rect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("clip: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    m_view->ClipToRect(rect);
}

void GraphicsContextHaiku::clipPath(const Path& path, WindRule windRule)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("clipPath: (--todo print values)\n"));
    m_view->SetFillRule(windRule == WindRule::EvenOdd ? B_EVEN_ODD : B_NONZERO);
    m_view->ClipToShape(path.platformPath());
    m_view->SetFillRule(fillRule() == WindRule::NonZero ? B_NONZERO : B_EVEN_ODD);
}

void GraphicsContextHaiku::clipToImageBuffer(WebCore::ImageBuffer& imageBuffer, WebCore::FloatRect const& destRect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("clipToImageBuffer [%f:%f] [%f:%f]\n", destRect.x(), destRect.y(), destRect.width(), destRect.height()));
    auto nativeImage = imageBuffer.createNativeImageReference();
    if(!nativeImage)
        return;
    
    BPicture picture;
    m_view->BeginPicture(&picture);
    BBitmap* bmp = nativeImage->platformImage().get();
    
	m_view->SetDrawingMode(B_OP_ALPHA);
	m_view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    m_view->SetHighColor(0,0,0,0);
    m_view->FillRect(destRect);
    m_view->DrawBitmap(bmp, destRect);
    m_view->EndPicture();
    m_view->ClipToPicture(&picture);
}

void GraphicsContextHaiku::resetClip()
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("resetClip\n"));
    m_view->ClipToRect(m_view->Bounds());
}


void GraphicsContextHaiku::drawPattern(const NativeImage& image, const FloatRect& destRect,
    const FloatRect& tileRect, const AffineTransform& transform,
    const FloatPoint& phase, const FloatSize& spacing, ImagePaintingOptions options)
{
    PaintTimerHaiku imageTimer(paintStatisticsHaiku().imageTime);
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.images;
    HGTRACE(("drawPattern: (--todo print values)\n"));
    drawBitmap(image.platformImage().get(), image.size(), destRect, tileRect, transform, phase, spacing, options);
}

void GraphicsContextHaiku::drawBitmap(BBitmap* image, const WebCore::FloatSize& size, const FloatRect& destRect,
    const FloatRect& tileRect, const AffineTransform&,
    const FloatPoint& phase, const FloatSize& spacing, const ImagePaintingOptions&)
{
    HGTRACE(("drawBitmap: (--todo print values)\n"));
    if (!image->IsValid()) // If the image hasn't fully loaded.
        return;

    m_view->PushState();

    clip(enclosingIntRect(destRect));
    float phaseOffsetX = destRect.x() - phase.x();
    float phaseOffsetY = destRect.y() - phase.y();
    // x mod w, y mod h
    phaseOffsetX -= std::trunc(phaseOffsetX / tileRect.width()) * tileRect.width();
    phaseOffsetY -= std::trunc(phaseOffsetY / tileRect.height()) * tileRect.height();
    m_view->DrawTiledBitmapAsync(
        image, destRect, BPoint(phaseOffsetX, phaseOffsetY));
    m_view->PopState();
}


void GraphicsContextHaiku::clipOut(const Path& path)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("clipOut(path): (--todo print values)\n"));
    if (path.isEmpty())
        return;

    m_view->ClipToInverseShape(path.platformPath());
}

void GraphicsContextHaiku::clipOut(const FloatRect& rect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().clipTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.clips;
    HGTRACE(("clipOut: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    m_view->ClipToInverseRect(rect);
}

void GraphicsContextHaiku::drawFocusRing(const Path& path, float width, const Color& color, float zoomFactor)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("drawFocusRing(path): (--todo print values)\n"));
    if (width <= 0 || !color.isVisible())
        return;

    // GTK forces this to 2, we use 1. A focus ring several pixels thick doesn't
    // look good.
    width = 1;

    m_view->SetHighColor(color);
    m_view->SetPenSize(width);
    m_view->StrokeShape(path.platformPath(), B_SOLID_HIGH);
}

void GraphicsContextHaiku::drawFocusRing(const Vector<FloatRect>& rects, float width, const Color& color, float zoomFactor)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("drawFocusRing(rects): (--todo print values)\n"));
    if (width <= 0 || !color.isVisible())
        return;

    unsigned rectCount = rects.size();
    if (rectCount <= 0)
        return;

    // GTK forces this to 2, we use 1. A focus ring several pixels thick doesn't
    // look good.
    // FIXME this still draws a focus ring that looks not so good on "details"
    // elements. Maybe we should disable that somewhere.
    width = 1;

    m_view->SetHighColor(color);
    m_view->SetPenSize(width);
    // FIXME: maybe we should implement this with BShape?
    for (unsigned i = 0; i < rectCount; ++i)
        m_view->StrokeRect(rects[i], B_SOLID_HIGH);
}

void GraphicsContextHaiku::drawLinesForText(const FloatPoint& point,
    float thickness, const std::span<const FloatSegment> widths, bool printing,
    bool doubleUnderlines, WebCore::StrokeStyle style)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("drawLinesForText: (--todo print values)\n"));
    if (widths.empty() || style == WebCore::StrokeStyle::NoStroke)
        return;

    Color lineColor(strokeColor());
    FloatRect bounds = computeLineBoundsAndAntialiasingModeForText(
        FloatRect(point, FloatSize(widths.end()->end, thickness)),
        printing, lineColor);
    if (bounds.isEmpty() || !strokeColor().isVisible())
        return;

    float y = bounds.center().y();

    m_view->SetPenSize(bounds.height());

    // TODO would be faster to use BeginLineArray/EndLineArray here
    for (const auto& width: widths)
    {
        m_view->StrokeLine(BPoint(bounds.x() + width.begin, y),
            BPoint(bounds.x() + width.end, y));
    }
    m_view->SetPenSize(strokeThickness());
}

void GraphicsContextHaiku::drawDotsForDocumentMarker(WebCore::FloatRect const&,
	WebCore::DocumentMarkerLineStyle)
{
    PrimitiveTimerHaiku primitiveTimer;
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.strokes;
    HGTRACE(("drawDotsForDocumentMarker: Not Implemented\n"));
	notImplemented();
}

/* Used by canvas.clearRect. Must clear the given rectangle with transparent black. */
void GraphicsContextHaiku::clearRect(const FloatRect& rect)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().fillTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.fills;
    HGTRACE(("clearRect: [%f:%f] [%f:%f]\n", rect.x(), rect.y(), rect.width(), rect.height()));
    m_view->SetHighColor(0, 0, 0, 0);
    m_view->SetDrawingMode(B_OP_COPY);
    m_view->FillRect(rect);
    m_view->SetDrawingMode(B_OP_ALPHA);
}

void GraphicsContextHaiku::setLineCap(LineCap lineCap)
{
    HGTRACE(("setLineCap: (--todo print values)\n"));
    cap_mode mode = B_BUTT_CAP;
    switch (lineCap) {
    case LineCap::Round:
        mode = B_ROUND_CAP;
        break;
    case LineCap::Square:
        mode = B_SQUARE_CAP;
        break;
    case LineCap::Butt:
    default:
        break;
    }

    m_view->SetLineMode(mode, m_view->LineJoinMode(), m_view->LineMiterLimit());
}

void GraphicsContextHaiku::setLineDash(const DashArray& /*dashes*/, float /*dashOffset*/)
{
    HGTRACE(("setLineDash: Not Implemented\n"));
    // TODO this is used to draw dashed strokes in SVG, but we need app_server support
    notImplemented();
}

void GraphicsContextHaiku::setLineJoin(LineJoin lineJoin)
{
    HGTRACE(("setLineJoin: (--todo print values)\n"));
    join_mode mode = B_MITER_JOIN;
    switch (lineJoin) {
    case LineJoin::Round:
        mode = B_ROUND_JOIN;
        break;
    case LineJoin::Bevel:
        mode = B_BEVEL_JOIN;
        break;
    case LineJoin::Miter:
    default:
        break;
    }

    m_view->SetLineMode(m_view->LineCapMode(), mode, m_view->LineMiterLimit());
}

void GraphicsContextHaiku::setMiterLimit(float limit)
{
    HGTRACE(("setMiterLimit: %f\n", limit));
    m_view->SetLineMode(m_view->LineCapMode(), m_view->LineJoinMode(), limit);
}

AffineTransform GraphicsContextHaiku::getCTM(IncludeDeviceScale) const
{
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.roundTrips;
    PaintTimerHaiku roundTripTimer(paintStatisticsHaiku().roundTripTime);
    BAffineTransform t = m_view->Transform();
    HGTRACE(("getCTM: out[%f %f %f %f %f %f]\n", t.sx, t.shy, t.shx, t.sy, t.tx, t.ty));
    	// TODO: we actually need to use the combined transform here?
    AffineTransform matrix(t.sx, t.shy, t.shx, t.sy, t.tx, t.ty);
    return matrix;
}

void GraphicsContextHaiku::translate(float x, float y)
{
    HGTRACE(("translate: %f, %f\n", x, y));
    if (x == 0.f && y == 0.f)
        return;

    m_view->TranslateBy(x, y);
}

void GraphicsContextHaiku::rotate(float radians)
{
    HGTRACE(("rotate: %f\n", radians));
    if (radians == 0.f)
        return;

    m_view->RotateBy(radians);
}

void GraphicsContextHaiku::scale(const FloatSize& size)
{
    HGTRACE(("scale: %f %f\n", size.width(), size.height()));
    m_view->ScaleBy(size.width(), size.height());
}

void GraphicsContextHaiku::concatCTM(const AffineTransform& transform)
{
    HGTRACE(("concatCTM: (--todo print values)\n"));
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.roundTrips;
    PaintTimerHaiku roundTripTimer(paintStatisticsHaiku().roundTripTime);
    BAffineTransform current = m_view->Transform();
    current.Multiply(transform);
    m_view->SetTransform(current);
}

void GraphicsContextHaiku::setCTM(const AffineTransform& transform)
{
    HGTRACE(("setCTM: (--todo print values)\n"));
    m_view->SetTransform(transform);
}

void GraphicsContextHaiku::didUpdateState(GraphicsContextState& state)
{
    if(state.changes().isEmpty()) {
        state.didApplyChanges();
        return;
    }
        
#ifdef TRACE_GRAPHICS_HAIKU
    // Building this dump means formatting every changed property into a string,
    // and didUpdateState() runs for every fill colour, stroke and alpha a page
    // sets. It used to be built in release builds too, only to be dropped.
    TextStream ts;
    ts << "didUpdateState ";
    state.dump(ts);
    HGTRACE(("%s\n", ts.release().ascii().data()));
#endif
    auto& paintStatistics = paintStatisticsHaiku();
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku stateTimer(paintStatistics.stateTime);
    if (paintStatistics.enabled)
        ++paintStatistics.stateChanges;
#if 0
        FillBrush                   = 1 << 0,
        FillRule                    = 1 << 1,

        StrokeBrush                 = 1 << 2,
        StrokeThickness             = 1 << 3,
        StrokeStyle                 = 1 << 4,
        
        Alpha                       = 1 << 8,
#endif
    if (state.changes().contains(GraphicsContextState::Change::StrokeThickness)) {
        m_view->SetPenSize(state.strokeThickness());
    }
    if (state.changes().contains(GraphicsContextState::Change::StrokeBrush)) {
        // FIXME the alpha is only applied to plain colors, not bitmaps or gradients.
        m_view->SetHighColor(withGlobalAlpha(state.strokeBrush().color(), state.alpha()));
    }
    if (state.changes().contains(GraphicsContextState::Change::StrokeStyle)) {
        switch (strokeStyle()) {
			case WebCore::StrokeStyle::DoubleStroke:
			case WebCore::StrokeStyle::WavyStroke:
                notImplemented();
                m_strokeStyle = B_SOLID_HIGH;
                break;
			case WebCore::StrokeStyle::SolidStroke:
                m_strokeStyle = B_SOLID_HIGH;
                break;
			case WebCore::StrokeStyle::DottedStroke:
                m_view->SetLowColor(B_TRANSPARENT_COLOR);
                m_strokeStyle = B_MIXED_COLORS;
                break;
			case WebCore::StrokeStyle::DashedStroke:
                // FIXME: use a better dashed stroke!
                notImplemented();
                m_view->SetLowColor(B_TRANSPARENT_COLOR);
                m_strokeStyle = B_MIXED_COLORS;
                break;
            case WebCore::StrokeStyle::NoStroke:
                m_strokeStyle = B_SOLID_LOW;
                break;
        }
    }
    if (state.changes().contains(GraphicsContextState::Change::FillBrush)) {
        m_view->SetLowColor(withGlobalAlpha(state.fillBrush().color(), state.alpha()));
    }
    if (state.changes().contains(GraphicsContextState::Change::FillRule))
        m_view->SetFillRule(fillRule() == WindRule::NonZero ? B_NONZERO : B_EVEN_ODD);
#if 0
        CompositeMode               = 1 << 5,
        DropShadow                  = 1 << 6,
        Style                       = 1 << 7,
#endif
    if (state.changes().contains(GraphicsContextState::Change::Alpha)) {
        // Recompute from the brushes: replacing the view colors' alpha lost translucent colors.
        m_view->SetHighColor(withGlobalAlpha(this->state().strokeBrush().color(), state.alpha()));
        m_view->SetLowColor(withGlobalAlpha(this->state().fillBrush().color(), state.alpha()));
    }
    if (state.changes().contains(GraphicsContextState::Change::DropShadow)) {
        /*drawing_mode mode = B_OP_ALPHA;
        alpha_function blending_mode = B_ALPHA_COMPOSITE_SOURCE_IN;
        m_view->SetDrawingMode(mode);
        m_view->SetBlendingMode(B_PIXEL_ALPHA, blending_mode);*/
    }
    if (state.changes().contains(GraphicsContextState::Change::CompositeMode)) {
        drawing_mode mode = B_OP_ALPHA;
        alpha_function blending_mode = B_ALPHA_COMPOSITE;
        switch (compositeOperation()) {
            case CompositeOperator::SourceOver:
                blending_mode = B_ALPHA_COMPOSITE_SOURCE_OVER;
                break;
            case CompositeOperator::PlusLighter:
                blending_mode = B_ALPHA_COMPOSITE_LIGHTEN;
                break;
            case CompositeOperator::Difference:
                blending_mode = B_ALPHA_COMPOSITE_DIFFERENCE;
                break;
            case CompositeOperator::PlusDarker:
                blending_mode = B_ALPHA_COMPOSITE_DARKEN;
                break;
            case CompositeOperator::Clear:
                blending_mode = B_ALPHA_COMPOSITE_CLEAR;
                break;
            case CompositeOperator::DestinationOut:
                blending_mode = B_ALPHA_COMPOSITE_DESTINATION_OUT;
                break;
            case CompositeOperator::SourceAtop:
                blending_mode = B_ALPHA_COMPOSITE_SOURCE_ATOP;
                break;
            case CompositeOperator::SourceIn:
                blending_mode = B_ALPHA_COMPOSITE_SOURCE_IN;
                break;
            case CompositeOperator::SourceOut:
                blending_mode = B_ALPHA_COMPOSITE_SOURCE_OUT;
                break;
            case CompositeOperator::DestinationOver:
                blending_mode = B_ALPHA_COMPOSITE_DESTINATION_OVER;
                break;
            case CompositeOperator::DestinationAtop:
                blending_mode = B_ALPHA_COMPOSITE_DESTINATION_ATOP;
                break;
            case CompositeOperator::DestinationIn:
                blending_mode = B_ALPHA_COMPOSITE_DESTINATION_IN;
                break;
            case CompositeOperator::XOR:
                blending_mode = B_ALPHA_COMPOSITE_XOR;
                break;
            case CompositeOperator::Copy:
                mode = B_OP_COPY;
                break;
            default:
                fprintf(stderr, "GraphicsContext::setCompositeOperation: Unsupported composite operation %s\n",
                        compositeOperatorName(compositeMode().operation, compositeMode().blendMode).utf8().legacyCStringPointer());
        }
        m_view->SetDrawingMode(mode);
        m_view->SetBlendingMode(B_PIXEL_ALPHA, blending_mode);
    }
#if 0
        TextDrawingMode             = 1 << 9,
        ImageInterpolationQuality   = 1 << 10,
        
        ShouldAntialias             = 1 << 11,
        ShouldSmoothFonts           = 1 << 12,
        ShouldSubpixelQuantizeFonts = 1 << 13,
        ShadowsIgnoreTransforms     = 1 << 14,
        DrawLuminanceMask           = 1 << 15,
#endif

    state.didApplyChanges();
}

#if ENABLE(3D_RENDERING) && USE(TEXTURE_MAPPER)
TransformationMatrix GraphicsContextHaiku::get3DTransform() const
{
    // FIXME: Can we approximate the transformation better than this?
    return getCTM().toTransformationMatrix();
}

void GraphicsContextHaiku::concat3DTransform(const TransformationMatrix& transform)
{
    concatCTM(transform.toAffineTransform());
}

void GraphicsContextHaiku::set3DTransform(const TransformationMatrix& transform)
{
    setCTM(transform.toAffineTransform());
}
#endif

void GraphicsContextHaiku::beginTransparencyLayer(float opacity)
{
    HGTRACE(("beginTransparencyLayer: %f\n", opacity));
    GraphicsContext::beginTransparencyLayer(opacity);
    // app_server sizes a layer from the bounding box of the operations recorded in it, and
    // that pass ignores strings drawn at glyph offsets and bitmaps. Text and images in a
    // translucent element were clipped away unless other shapes happened to surround them.
    // Record a fully transparent fill first so the layer always covers the clipped area.
    BRect layerBounds(clipBounds());
    save(GraphicsContextState::Purpose::TransparencyLayer);
    m_view->BeginLayer(static_cast<uint8>(opacity * 255.0));
    m_view->PushState();
    m_view->SetDrawingMode(B_OP_ALPHA);
    m_view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    m_view->SetHighColor(0, 0, 0, 0);
    m_view->FillRect(layerBounds);
    m_view->PopState();
}

void GraphicsContextHaiku::endTransparencyLayer()
{
    HGTRACE(("endTransparencyLayer: no values\n"));
    GraphicsContext::endTransparencyLayer();
    m_view->EndLayer();
    restore(GraphicsContextState::Purpose::TransparencyLayer);
}

IntRect GraphicsContextHaiku::clipBounds() const
{
    HGTRACE(("clipBounds: no values\n"));
    // This can be used by drawing code to do some early clipping (for example
    // the SVG code may skip complete parts of the image which are outside
    // the bounds).
    // So, we get the current clipping region, and convert it back to drawing
    // space by applying the reverse of the view transform.

    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled) {
        ++paintStatistics.roundTrips;
        ++paintStatistics.clipQueries;
    }
    BRegion region;
    {
        PaintTimerHaiku roundTripTimer(paintStatisticsHaiku().roundTripTime);
        m_view->GetClippingRegion(&region);
    }
    BRect rect = region.Frame();

    BPoint points[4];
    points[0] = rect.LeftTop();
    points[1] = rect.RightBottom();
    points[2] = rect.LeftBottom();
    points[3] = rect.RightTop();

    BAffineTransform t = m_view->TransformTo(B_VIEW_COORDINATES);
    t.ApplyInverse(points, 4);

    rect.left   = std::min({points[0].x, points[1].x, points[2].x, points[3].x});
    rect.right  = std::max({points[0].x, points[1].x, points[2].x, points[3].x});
    rect.top    = std::min({points[0].y, points[1].y, points[2].y, points[3].y});
    rect.bottom = std::max({points[0].y, points[1].y, points[2].y, points[3].y});

    return IntRect(rect);
}


void GraphicsContextHaiku::save(GraphicsContextState::Purpose)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().saveTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.stateChanges;
    HGTRACE(("save: no values\n"));
    m_view->PushState();
    GraphicsContext::save();
}

void GraphicsContextHaiku::restore(GraphicsContextState::Purpose)
{
    PrimitiveTimerHaiku primitiveTimer;
    PaintTimerHaiku bucketTimer(paintStatisticsHaiku().saveTime);
    if (auto& paintStatistics = paintStatisticsHaiku(); paintStatistics.enabled)
        ++paintStatistics.stateChanges;
    HGTRACE(("restore: no values\n"));
    GraphicsContext::restore();
    m_view->PopState();
}


} // namespace WebCore
