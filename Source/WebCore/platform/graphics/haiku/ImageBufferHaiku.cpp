/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
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
#include "ImageBufferHaikuSurfaceBackend.h"

#include "BitmapImage.h"
#include "ColorUtilities.h"
#include "GraphicsContextHaiku.h"
#include "ImageData.h"
#include "IntRect.h"
#include "MIMETypeRegistry.h"
#include "NotImplemented.h"
#include "PixelBuffer.h"
#include "PixelBufferConversion.h"
#include "JavaScriptCore/JSCInlines.h"
#include "JavaScriptCore/TypedArrayInlines.h"

#include <wtf/text/Base64.h>
#include <wtf/text/CString.h>
#include <wtf/FastMalloc.h>
#include <BitmapStream.h>
#include <Picture.h>
#include <String.h>
#include <Window.h>
#include <cstdlib>
#include <cstring>
#include <TranslatorRoster.h>
#include <stdio.h>


namespace WebCore {


ImageBufferData::ImageBufferData(const IntSize& size)
    : m_view(NULL)
    , m_context(NULL)
    , m_image(adoptRef(new BitmapRef(BRect(0, 0, size.width() - 1, size.height() - 1), B_RGBA32, true)))
{
    // Always keep the bitmap locked, we are the only client.
    m_image->Lock();
    if(size.isEmpty())
        return;

    if (!m_image->IsLocked() || !m_image->IsValid())
        return;

    m_view = new BView(m_image->Bounds(), "WebKit ImageBufferData", 0, 0);
    m_image->AddChild(m_view);

    // Fill with completely transparent color.
    memset(m_image->Bits(), 0, m_image->BitsLength());

    // Since ImageBuffer is used mainly for Canvas, explicitly initialize
    // its view's graphics state with the corresponding canvas defaults
    // NOTE: keep in sync with CanvasRenderingContext2D::State
    m_view->SetLineMode(B_BUTT_CAP, B_MITER_JOIN, 10);
    m_view->SetDrawingMode(B_OP_ALPHA);
    m_view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    m_context = new GraphicsContextHaiku(m_view);
}


ImageBufferData::~ImageBufferData()
{
    m_view = nullptr;
        // m_bitmap owns m_view and deletes it when going out of this destructor.
    m_image = nullptr;
    delete m_context;
}


// BView flushes its link to app_server on every PopState(), and WebKit saves
// and restores state constantly. A view transaction holds those flushes back
// until the drawing is finished, so the commands travel in whole buffers
// instead of one port write each. SUMMIT_BATCH_DRAWING=0 turns it off.
static bool batchedDrawingEnabledHaiku()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_BATCH_DRAWING");
        return !value || strcmp(value, "0");
    }();
    return enabled;
}

void ImageBufferHaikuSurfaceBackend::endBatch()
{
    if (!m_batching)
        return;
    m_batching = false;
    if (BWindow* window = m_data.m_view ? m_data.m_view->Window() : nullptr)
        window->EndViewTransaction();
}

void ImageBufferHaikuSurfaceBackend::syncDrawing()
{
    endBatch();
    if (!m_drawingPending || !m_data.m_view)
        return;
    m_data.m_view->Sync();
    m_drawingPending = false;
}


WTF::RefPtr<WebCore::NativeImage> ImageBufferHaikuSurfaceBackend::copyNativeImage()
{
    syncDrawing();

    // This actually creates a new BBitmap and copies the data
    BitmapRef* ref = new BitmapRef(*(BBitmap*)m_data.m_image.get());
    return NativeImage::create(ref);
}


WTF::RefPtr<WebCore::NativeImage> ImageBufferHaikuSurfaceBackend::createNativeImageReference()
{
    syncDrawing();

    // This just creates a new reference to the existing BBitmap
    PlatformImagePtr ref = m_data.m_image;
    return NativeImage::create(std::move(ref));
}


std::unique_ptr<ImageBufferHaikuSurfaceBackend>
ImageBufferHaikuSurfaceBackend::create(const ImageBufferParameters& parameters,
    const ImageBufferCreationContext&)
{
    auto backendSize = calculateSafeBackendSize(parameters);
    if (backendSize.isEmpty())
        return nullptr;

    auto backend = std::unique_ptr<ImageBufferHaikuSurfaceBackend>(
        new ImageBufferHaikuSurfaceBackend(parameters, backendSize));
    if (!backend->m_data.m_context)
        return nullptr;
    return backend;
}


std::unique_ptr<ImageBufferHaikuSurfaceBackend>
ImageBufferHaikuSurfaceBackend::create(const ImageBufferParameters& parameters,
    const GraphicsContext&)
{
    return create(parameters, ImageBufferCreationContext());
}


ImageBufferHaikuSurfaceBackend::ImageBufferHaikuSurfaceBackend(
    const ImageBufferParameters& parameters, const IntSize& backendSize)
    : ImageBufferBackend(parameters)
    , m_data(backendSize)
{
}

ImageBufferHaikuSurfaceBackend::ImageBufferHaikuSurfaceBackend(
    const ImageBufferParameters& parameters)
    : ImageBufferBackend(parameters)
    , m_data({0, 0})
{
}

ImageBufferHaikuSurfaceBackend::~ImageBufferHaikuSurfaceBackend()
{
    endBatch();
}

GraphicsContext& ImageBufferHaikuSurfaceBackend::context()
{
    // The caller asked for the context to draw with, so the pixels owe
    // app_server a Sync() before anything reads them again.
    m_drawingPending = true;
    if (!m_batching && batchedDrawingEnabledHaiku() && m_data.m_view) {
        if (BWindow* window = m_data.m_view->Window()) {
            window->BeginViewTransaction();
            m_batching = true;
        }
    }
    return *m_data.m_context;
}


void ImageBufferHaikuSurfaceBackend::getPixelBuffer(
    const IntRect& srcRect, PixelBuffer& destination)
{
    syncDrawing();

    auto clipped = intersection(IntRect { { }, size() }, srcRect);
    if (clipped.size() != srcRect.size())
        destination.zeroFill();
    if (clipped.isEmpty())
        return;

    auto sourceData = std::span<const uint8_t>(static_cast<const uint8_t*>(m_data.m_image->Bits()), m_data.m_image->BitsLength());
    ConstPixelBufferConversionView source {
        // B_RGBA32 stores straight alpha. The common backend helper assumes
        // premultiplied storage and would divide these colors a second time.
        { AlphaPremultiplication::Unpremultiplied, pixelFormat(), colorSpace() },
        bytesPerRow(),
        sourceData.subspan(clipped.y() * bytesPerRow() + clipped.x() * PixelBuffer::bytesPerPixel(pixelFormat()))
    };
    auto destinationBytesPerPixel = PixelBuffer::bytesPerPixel(destination.format().pixelFormat);
    auto destinationBytesPerRow = uint32_t(srcRect.width()) * destinationBytesPerPixel;
    auto offset = (clipped.y() - srcRect.y()) * destinationBytesPerRow + (clipped.x() - srcRect.x()) * destinationBytesPerPixel;
    if (offset > destination.bytes().size())
        return;
    PixelBufferConversionView target { destination.format(), destinationBytesPerRow, destination.bytes().subspan(offset) };
    convertImagePixels(source, target, clipped.size());
}

void ImageBufferHaikuSurfaceBackend::putPixelBuffer(const PixelBufferSourceView& imageData, const IntRect& sourceRect, const IntPoint& destPoint, AlphaPremultiplication)
{
    // Writing the pixels directly: app_server must be finished with them, and
    // what this writes is not something app_server has to be told about.
    syncDrawing();
    // The native renderer and bitmap translators both consume straight alpha,
    // regardless of the caller's default for other ImageBuffer backends.
    ImageBufferBackend::putPixelBuffer(imageData, sourceRect, destPoint, AlphaPremultiplication::Unpremultiplied,
        std::span<unsigned char>(reinterpret_cast<unsigned char*>(m_data.m_image->Bits()), m_data.m_image->BitsLength()));
}

unsigned ImageBufferHaikuSurfaceBackend::bytesPerRow() const
{
    return m_data.m_image->BytesPerRow();
}


// TODO: quality
Vector<uint8_t> platformEncodeData(const NativeImage& image, const String& mimeType, std::optional<double> /*quality*/)
{
    auto* bitmap = image.platformImage().get();
    if (!bitmap)
        return { };

    BString mimeTypeString(mimeType);

    uint32 translatorType = 0;

    BTranslatorRoster* roster = BTranslatorRoster::Default();
    translator_id* translators = NULL;
    int32 translatorCount;
    roster->GetAllTranslators(&translators, &translatorCount);
    for (int32 i = 0; i < translatorCount; i++) {
        // Skip translators that don't support archived BBitmaps as input data.
        const translation_format* inputFormats;
        int32 formatCount;
        roster->GetInputFormats(translators[i], &inputFormats, &formatCount);
        bool supportsBitmaps = false;
        for (int32 j = 0; j < formatCount; j++) {
            if (inputFormats[j].type == B_TRANSLATOR_BITMAP) {
                supportsBitmaps = true;
                break;
            }
        }
        if (!supportsBitmaps)
            continue;

        const translation_format* outputFormats;
        roster->GetOutputFormats(translators[i], &outputFormats, &formatCount);
        for (int32 j = 0; j < formatCount; j++) {
            if (outputFormats[j].group == B_TRANSLATOR_BITMAP
                && mimeTypeString == outputFormats[j].MIME) {
                translatorType = outputFormats[j].type;
            }
        }
        if (translatorType)
            break;
    }
    delete[] translators;


    BMallocIO translatedStream;
    BBitmapStream bitmapStream(bitmap);
    BBitmap* tmp = NULL;
    if (roster->Translate(&bitmapStream, 0, 0, &translatedStream, translatorType,
                          B_TRANSLATOR_BITMAP, mimeType.utf8().legacyCStringPointer()) != B_OK) {
        bitmapStream.DetachBitmap(&tmp);
        return Vector<uint8_t>();
    }

    bitmapStream.DetachBitmap(&tmp);

    // FIXME we could use a BVectorIO to avoid an extra copy here
    Vector<uint8_t> result;
    off_t size;
    translatedStream.GetSize(&size);
    result.append(std::span<uint8_t>((uint8_t*)translatedStream.Buffer(), size));

    return result;
}

size_t ImageBufferHaikuSurfaceBackend::calculateMemoryCost(const ImageBufferParameters& parameters)
{
    auto backendSize = calculateSafeBackendSize(parameters);
    if (backendSize.isEmpty())
        return 0;
    auto bytesPerRow = CheckedUint32(backendSize.width()) * 4;
    return ImageBufferBackend::calculateMemoryCost(backendSize, bytesPerRow);
}

} // namespace WebCore
