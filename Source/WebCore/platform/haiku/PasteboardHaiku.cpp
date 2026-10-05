/*
 * Copyright (C) 2006 Zack Rusin <zack@kde.org>
 * Copyright (C) 2007 Ryan Leavengood <leavengood@gmail.com>
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
#include "Pasteboard.h"

#include "Blob.h"
#include "DOMURL.h"
#include "Document.h"
#include "DocumentFragment.h"
#include "DragData.h"
#include "Image.h"
#include "ImageUtilities.h"
#include "NativeImage.h"
#include "SharedBuffer.h"
#include "Editor.h"
#include "Frame.h"
#include "LocalFrameInlines.h"
#include "wtf/URL.h"
#include "NotImplemented.h"
#include "SimpleRange.h"
#include "TextResourceDecoder.h"
#include "markup.h"

#include <support/Locker.h>
#include <app/Clipboard.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <Message.h>
#include <String.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <wtf/text/CString.h>


namespace WebCore {

    std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return std::make_unique<Pasteboard>(std::move(context));
}

#if ENABLE(DRAG_SUPPORT)
// A drag has a pasteboard of its own: a drag that starts in a page used to
// write the system clipboard, and a drop read it.
std::unique_ptr<Pasteboard> Pasteboard::createForDragAndDrop(std::unique_ptr<PasteboardContext>&& context)
{
    return std::make_unique<Pasteboard>(std::move(context), Vector<String> { });
}

std::unique_ptr<Pasteboard> Pasteboard::create(const DragData& dragData)
{
    return std::make_unique<Pasteboard>(dragData.createPasteboardContext(), dragData.asFilenames());
}
#endif

Pasteboard::Pasteboard(std::unique_ptr<WebCore::PasteboardContext, std::default_delete<WebCore::PasteboardContext> >&& context)
    : m_context(std::move(context))
{
}

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context, Vector<String>&& dragFileNames)
    : m_context(std::move(context))
    , m_forDragAndDrop(true)
    , m_dragFileNames(std::move(dragFileNames))
{
}


// BClipboard unfortunately does not derive from BLocker, so we cannot use BAutolock.
class AutoClipboardLocker {
public:
    AutoClipboardLocker(BClipboard* clipboard)
        : m_clipboard(clipboard)
        , m_isLocked(clipboard && clipboard->Lock())
    {
    }

    ~AutoClipboardLocker()
    {
        if (m_isLocked)
            m_clipboard->Unlock();
    }

    bool isLocked() const
    {
        return m_isLocked;
    }

private:
    BClipboard* m_clipboard;
    bool m_isLocked;
};

// Images on the clipboard: PNG or other image data under its MIME type, or a
// BBitmap archive under "image/bitmap" (Screenshot, ShowImage) or
// "image/x-be-bitmap". Call with the clipboard locked.
static bool clipboardHasImage(BMessage* data)
{
    if (!data)
        return false;
    if (data->HasMessage("image/bitmap") || data->HasMessage("image/x-be-bitmap"))
        return true;
    char* name;
    uint32 type;
    int32 count;
    for (int32 i = 0; data->GetInfo(B_MIME_TYPE, i, &name, &type, &count) == B_OK; i++) {
        if (!strncmp(name, "image/", 6))
            return true;
    }
    return false;
}

static RefPtr<SharedBuffer> encodePNG(BBitmap* bitmap)
{
    // BBitmapStream owns the bitmap until it is detached again.
    BBitmapStream stream(bitmap);
    BMallocIO output;
    status_t status = BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &output, B_PNG_FORMAT);
    BBitmap* detached = nullptr;
    stream.DetachBitmap(&detached);
    if (status != B_OK || !output.BufferLength())
        return nullptr;
    return SharedBuffer::create(std::span(static_cast<const uint8_t*>(output.Buffer()), output.BufferLength()));
}

struct ClipboardImage {
    String type;
    RefPtr<SharedBuffer> data;
};

// The clipboard's image as a file a page can take: PNG for bitmaps. Call with
// the clipboard locked.
static ClipboardImage readClipboardImage(BMessage* data)
{
    if (!data)
        return { };
    const void* bytes = nullptr;
    ssize_t length = 0;
    if (data->FindData("image/png", B_MIME_TYPE, &bytes, &length) == B_OK && length > 0)
        return { "image/png"_s, SharedBuffer::create(std::span(static_cast<const uint8_t*>(bytes), length)) };
    BMessage archive;
    if (data->FindMessage("image/bitmap", &archive) == B_OK || data->FindMessage("image/x-be-bitmap", &archive) == B_OK) {
        auto bitmap = std::make_unique<BBitmap>(&archive);
        if (bitmap->InitCheck() == B_OK && bitmap->IsValid()) {
            if (auto png = encodePNG(bitmap.get()))
                return { "image/png"_s, WTF::move(png) };
        }
    }
    char* name;
    uint32 type;
    int32 count;
    for (int32 i = 0; data->GetInfo(B_MIME_TYPE, i, &name, &type, &count) == B_OK; i++) {
        if (strncmp(name, "image/", 6) || data->FindData(name, B_MIME_TYPE, &bytes, &length) != B_OK || length <= 0)
            continue;
        return { String::fromLatin1(name), SharedBuffer::create(std::span(static_cast<const uint8_t*>(bytes), length)) };
    }
    return { };
}

static String fileNameForImageType(const String& type)
{
    if (type == "image/png"_s)
        return "image.png"_s;
    if (type == "image/jpeg"_s)
        return "image.jpg"_s;
    if (type == "image/gif"_s)
        return "image.gif"_s;
    if (type == "image/webp"_s)
        return "image.webp"_s;
    return "image"_s;
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&)
{
    notImplemented();
}

void Pasteboard::writeString(const String& type, const String& data)
{
    if (m_forDragAndDrop)
        return;
    if (be_clipboard->Lock()) {
        bool result = false;
        BMessage* bdata = be_clipboard->Data();

        if (bdata) {
            CString typeUTF8 = type.utf8();
            bdata->RemoveName(typeUTF8.data());

            CString dataUTF8 = data.utf8();
            if (bdata->AddData(typeUTF8.data(), B_MIME_TYPE,
                    dataUTF8.data(), dataUTF8.length()) == B_OK)
                result = true;
        }

        if (result)
            be_clipboard->Commit();
        else
            be_clipboard->Revert();
        be_clipboard->Unlock();
    }
}

void Pasteboard::writeSelection(const std::optional<SimpleRange>& selectedRange, bool canSmartCopyOrDelete, LocalFrame& frame, ShouldSerializeSelectedTextForDataTransfer)
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    BString string(frame.editor().selectedText());

    // Replace unwanted representation of blank lines
    const char* utf8BlankLine = "\302\240\n";
    string.ReplaceAll(utf8BlankLine, "\n");

    data->AddData("text/plain", B_MIME_TYPE, string.String(), string.Length());

    BString markupString(serializePreservingVisualAppearance(*selectedRange, nullptr, AnnotateForInterchange::Yes));
    data->AddData("text/html", B_MIME_TYPE, markupString.String(), markupString.Length());

    be_clipboard->Commit();
}

void Pasteboard::writePlainText(const String& text, SmartReplaceOption smartReplaceOption)
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    BString string(text);
    data->AddData("text/plain", B_MIME_TYPE, string.String(), string.Length());
    be_clipboard->Commit();
}


// Copy Image: PNG data for pages and other browsers, and a BBitmap archive
// as Haiku applications (ShowImage, WonderBrush) paste it.
void WebCore::Pasteboard::write(WebCore::PasteboardImage const& pasteboardImage)
{
    if (m_forDragAndDrop || !pasteboardImage.image)
        return;
    RefPtr nativeImage = pasteboardImage.image->nativeImage();
    if (!nativeImage)
        return;
    auto png = encodeData(*nativeImage, "image/png"_s);
    if (png.isEmpty())
        return;

    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;
    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    if (!data)
        return;
    data->AddData("image/png", B_MIME_TYPE, png.span().data(), png.size());
    BMemoryIO input(png.span().data(), png.size());
    if (std::unique_ptr<BBitmap> bitmap { BTranslationUtils::GetBitmap(&input) }) {
        BMessage archive;
        if (bitmap->Archive(&archive) == B_OK)
            data->AddMessage("image/bitmap", &archive);
    }
    if (!pasteboardImage.url.url.isEmpty()) {
        CString url = pasteboardImage.url.url.string().utf8();
        data->AddData("text/uri-list", B_MIME_TYPE, url.data(), url.length());
    }
    be_clipboard->Commit();
}

void Pasteboard::write(const PasteboardBuffer&)
{
}

void WebCore::Pasteboard::write(WebCore::PasteboardWebContent const& content)
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    data->AddData("text/html", B_MIME_TYPE, content.markup.utf8().legacyCStringPointer(), content.markup.utf8().length());

    data->AddData("text/plain", B_MIME_TYPE, content.text.utf8().legacyCStringPointer(), content.text.utf8().length());

    be_clipboard->Commit();
}

void WebCore::Pasteboard::writeMarkup(WTF::String const& text)
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    BString string(text);
    data->AddData("text/html", B_MIME_TYPE, string.String(), string.Length());
    be_clipboard->Commit();
}



void Pasteboard::write(const PasteboardURL& url)
{
    if (m_forDragAndDrop)
        return;
    ASSERT(!url.url.isEmpty());

    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();

    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    BString string(url.url.string());
    data->AddData("text/plain", B_MIME_TYPE, string.String(), string.Length());
    be_clipboard->Commit();
}

void Pasteboard::write(const Color&)
{
}

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    if (m_forDragAndDrop)
        return m_dragFileNames.isEmpty() ? FileContentState::NoFileOrImageData : FileContentState::MayContainFilePaths;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return FileContentState::NoFileOrImageData;
    return clipboardHasImage(be_clipboard->Data()) ? FileContentState::InMemoryImage : FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    notImplemented();
    return false;
}


void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<long unsigned int>)
{
    notImplemented();
}


void Pasteboard::read(PasteboardPlainText& text, WebCore::PlainTextURLReadingPolicy, std::optional<long unsigned int>)
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    BMessage* data = be_clipboard->Data();
    if (!data)
        return;

    const char* buffer = 0;
    ssize_t bufferLength;
    if (data->FindData("text/plain", B_MIME_TYPE, 
            reinterpret_cast<const void**>(&buffer), &bufferLength) == B_OK)
        text.text = String::fromUTF8(std::span<const char>(buffer, bufferLength));
}

RefPtr<DocumentFragment> Pasteboard::documentFragment(LocalFrame& frame, const SimpleRange& context,
													  bool allowPlainText, bool& chosePlainText)
{
    chosePlainText = false;
    if (m_forDragAndDrop)
        return nullptr;

    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return nullptr;

    BMessage* data = be_clipboard->Data();
    if (!data)
        return nullptr;

    const char* buffer = 0;
    ssize_t bufferLength;
    if (data->FindData("text/html", B_MIME_TYPE, reinterpret_cast<const void**>(&buffer), &bufferLength) == B_OK) {
        RefPtr<TextResourceDecoder> decoder = TextResourceDecoder::create(ASCIILiteral::fromLiteralUnsafe("text/plain"), PAL::UTF8Encoding(), true);
        StringBuilder html;
        html.append(decoder->decode(std::span<const unsigned char>((const unsigned char*)buffer, bufferLength)));
        html.append(decoder->flush());

        if (!html.isEmpty()) {
            RefPtr<DocumentFragment> fragment = createFragmentFromMarkup(*frame.document(), html.toString(), String(), {});
            if (fragment)
                return fragment;
        }
    }

    if (auto image = readClipboardImage(data); image.data && frame.document()) {
        Ref document = *frame.document();
        auto url = DOMURL::createObjectURL(document, Blob::create(document.ptr(), image.data->extractData(), image.type));
        return createFragmentForImageAndURL(document, url, { });
    }

    if (!allowPlainText)
        return nullptr;

    if (data->FindData("text/plain", B_MIME_TYPE, reinterpret_cast<const void**>(&buffer), &bufferLength) == B_OK) {
        String plainText = String::fromUTF8(std::span<const char>(buffer, bufferLength));

        chosePlainText = true;
        RefPtr<DocumentFragment> fragment = createFragmentFromText(context, plainText);
        if (fragment)
            return fragment;
    }

    return nullptr;
}

bool Pasteboard::hasData()
{
    if (m_forDragAndDrop)
        return !m_dragFileNames.isEmpty();
    bool result = false;

    if (be_clipboard->Lock()) {
        BMessage* data = be_clipboard->Data();

        if (data)
            result = !data->IsEmpty();

        be_clipboard->Unlock();
    }

    return result;
}

void Pasteboard::clear(const String& type)
{
    if (m_forDragAndDrop)
        return;
    if (be_clipboard->Lock()) {
        BMessage* data = be_clipboard->Data();

        if (data) {
            data->RemoveName(BString(type).String());
            be_clipboard->Commit();
        }

        be_clipboard->Unlock();
    }
}

String Pasteboard::readOrigin()
{
    notImplemented(); // webkit.org/b/177633: [GTK] Move to new Pasteboard API
    return { };
}

String Pasteboard::readString(const String& type)
{
    if (m_forDragAndDrop)
        return { };
    String result;

    if (be_clipboard->Lock()) {
        BMessage* data = be_clipboard->Data();

        const char* buffer = nullptr;
        ssize_t bufferLength = 0;
        if (data && data->FindData(type.utf8().legacyCStringPointer(), B_MIME_TYPE,
                reinterpret_cast<const void**>(&buffer), &bufferLength) == B_OK && buffer && bufferLength > 0)
            result = String::fromUTF8(std::span<const char>(buffer, bufferLength));

        be_clipboard->Unlock();
    }

    return result;
}

String Pasteboard::readStringInCustomData(const String& type)
{
	return readString(type);
}

void Pasteboard::clear()
{
    if (m_forDragAndDrop)
        return;
    AutoClipboardLocker locker(be_clipboard);
    if (!locker.isLocked())
        return;

    be_clipboard->Clear();
    be_clipboard->Commit();
}

#if ENABLE(DRAG_SUPPORT)
void Pasteboard::setDragImage(DragImage, const IntPoint&)
{
    notImplemented();
}
#endif


// Only the web's own string types: the clipboard also holds BBitmap archives
// and application-private fields, and images reach pages as files.
Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    Vector<String> result;
    if (m_forDragAndDrop)
        return result;

    if (be_clipboard->Lock()) {
        BMessage* data = be_clipboard->Data();

        if (data) {
            char* name;
            uint32 type;
            int32 count;

            for (int32 i = 0; data->GetInfo(B_MIME_TYPE, i, &name, &type, &count) == B_OK; i++) {
                if (!strncmp(name, "text/", 5) || !strncmp(name, "application/", 12))
                    result.append(String::fromUTF8(name));
            }
        }

        be_clipboard->Unlock();
    }

    return result;
}

Vector<String> Pasteboard::typesSafeForBindings(const String&)
{
	return typesForLegacyUnsafeBindings();
}

void Pasteboard::writeCustomData(const WTF::Vector<PasteboardCustomData>& data, WebCore::PasteboardWriteType)
{
	if (!data.isEmpty()) {
		const auto& customData = data[0];
		customData.forEachPlatformString([this] (auto& type, auto& string) {
			writeString(type, string);
		});
	}
}

// Files for DataTransfer.files: the files of a drop, or the clipboard's
// image (a pasted screenshot becomes image.png).
void Pasteboard::read(WebCore::PasteboardFileReader& reader, std::optional<unsigned long> index)
{
    if (m_forDragAndDrop) {
        for (size_t i = 0; i < m_dragFileNames.size(); ++i) {
            if (!index || *index == i)
                reader.readFilename(m_dragFileNames[i]);
        }
        return;
    }
    if (index && *index)
        return;
    ClipboardImage image;
    {
        AutoClipboardLocker locker(be_clipboard);
        if (!locker.isLocked())
            return;
        image = readClipboardImage(be_clipboard->Data());
    }
    if (image.data && reader.shouldReadBuffer(image.type))
        reader.readBuffer(fileNameForImageType(image.type), image.type, image.data.releaseNonNull());
}


} // namespace WebCore

