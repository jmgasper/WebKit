/*
 * Copyright (C) 2026 Haiku, Inc. All rights reserved.
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

#pragma once

#include <MediaDecoder.h>
#include <MediaDefs.h>
#include <MediaTrack.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace BPrivate {
namespace media {
class Decoder;
}
}

// Haiku gives each encoded format to one decoder add-on, chosen by add-on
// directory, and a stream that add-on cannot decode does not fall back. The
// NVDEC add-on (the X399's GeForce, "nvdec h264") keeps 17 pictures on the
// card: an H.264 stream with 16 reference frames (level 5.1, common in
// high-quality 1080p files) runs out of them, and it skips to the next
// keyframe - 22 of 300 pictures, with freezes of seconds. Such streams go to
// libavcodec instead, picked by hand through the one public way to name a
// decoder add-on (BMediaDecoder::SetTo(const media_codec_info*)).
//
// The Raspberry Pi 4's own decoder is an add-on the media kit never offers:
// rpi_mmal (H.264 on the VideoCore firmware) registers no format, because a
// stream it refused would then have no other decoder. It is loaded by name,
// as the image's player airTime does, for the streams it is known to decode,
// and anything it refuses goes to the media kit's choice or libavcodec. Its
// pictures come as I420 and are converted to B_RGB32 here.

namespace WebCore {

// What the first sequence parameter set of an avcC record (ISO/IEC 14496-15)
// says about decoding the stream, or nullopt if the record cannot be read.
struct H264SequenceInfoHaiku {
    unsigned profile { 0 };
    unsigned chromaFormat { 1 };
    unsigned bitDepthLuma { 8 };
    unsigned bitDepthChroma { 8 };
    unsigned maxReferenceFrames { 0 };
    bool frameMbsOnly { true };
};
std::optional<H264SequenceInfoHaiku> h264SequenceInfo(std::span<const uint8_t> avcC);

// max_num_ref_frames of the first sequence parameter set in an avcC record,
// or nullopt if the record cannot be read.
std::optional<unsigned> h264MaxReferenceFrames(std::span<const uint8_t> avcC);

// Whether `decoder`, the add-on the media kit chose, cannot decode an H.264
// stream with this avcC. Only the NVDEC add-on under the name it has now
// ("nvdec h264") is suspected; a fixed add-on under another name is trusted.
// SUMMIT_NVDEC_MAX_REFERENCES overrides the 15 reference frames it handles.
bool hardwareDecoderCannotDecodeHaiku(const media_codec_info& decoder, std::span<const uint8_t> avcC);

// Sets `decoder` to an add-on other than NVDEC that accepts `format`
// (libavcodec in practice) and says which in `chosen`.
status_t setSoftwareDecoderHaiku(BMediaDecoder&, const media_format&, const void* info, size_t infoSize, media_codec_info* chosen = nullptr);

// A video decoder add-on the media kit does not offer, loaded by name (the
// Raspberry Pi's rpi_mmal), with its pictures converted to B_RGB32.
// SUMMIT_HARDWARE_VIDEO=0 never uses one.
class AddOnVideoDecoderHaiku {
    WTF_MAKE_NONCOPYABLE(AddOnVideoDecoderHaiku);
public:
    using ChunkSource = Function<status_t(const void**, size_t*, media_header*)>;

    // Whether an installed add-on is known to decode this encoded format
    // (with its avcC as `codecConfig`), before anything is read.
    static bool canDecode(const media_format& encoded, std::span<const uint8_t> codecConfig);
    // Null unless such an add-on takes the stream and decodes its first
    // picture, for which it reads chunks from `source`.
    static std::unique_ptr<AddOnVideoDecoderHaiku> create(const media_format& encoded, std::span<const uint8_t> codecConfig, ChunkSource&&);
    ~AddOnVideoDecoderHaiku();

    const media_codec_info& codecInfo() const { return m_codecInfo; }
    unsigned width() const { return m_width; }
    unsigned height() const { return m_height; }
    // The next picture, as `rowBytes` apart rows of B_RGB32. A picture before
    // the time a negative info->time_to_decode names is not converted.
    status_t decode(void* bgra, size_t rowBytes, int64* count, media_header*, media_decode_info* = nullptr);
    // The next picture as it comes, I420 (see I420Layout) in `planes`,
    // which holds planesSize() bytes.
    status_t decodePlanes(void* planes, int64* count, media_header*, media_decode_info* = nullptr);
    size_t planesSize() const { return m_planes.size(); }
    // The geometry of what decodePlanes() writes.
    struct I420Layout {
        unsigned width { 0 };
        unsigned height { 0 };
        size_t lumaRowBytes { 0 };
        size_t chromaOffset(unsigned plane) const { return lumaRowBytes * height + (plane - 1) * (lumaRowBytes / 2) * ((height + 1) / 2); }
        // BT.709 for pictures of 720 lines and more, BT.601 below, as
        // streams that say nothing else mean in practice.
        bool bt709() const { return height >= 720; }
    };
    I420Layout layout() const { return { m_width, m_height, m_planesRowBytes }; }
    // Converts I420 planes of `layout` into rows of B_RGB32 `rowBytes` apart.
    static void convertToBGRA(const uint8_t* planes, const I420Layout&, uint8_t* bgra, size_t rowBytes);
    // Forgets pictures on their way, after the source was moved.
    status_t seekedTo(int64 frame, bigtime_t);

private:
    class Forwarder;
    AddOnVideoDecoderHaiku(BPrivate::media::Decoder*, ChunkSource&&);

    // The source outlives the decoder, which may read from it until it is gone.
    ChunkSource m_source;
    std::unique_ptr<BPrivate::media::Decoder> m_decoder;
    media_codec_info m_codecInfo { };
    Vector<uint8_t> m_planes;
    size_t m_planesRowBytes { 0 };
    unsigned m_width { 0 };
    unsigned m_height { 0 };
};

// A BMediaTrack's pictures decoded by another decoder than the track's own:
// the Raspberry Pi's add-on, or libavcodec. Its encoded chunks are read raw.
// The frame counter gives the time, as libavcodec's frame times are in the
// stream's time base on Haiku.
class SubstituteVideoDecoderHaiku final : public BMediaDecoder {
public:
    // Null unless the Raspberry Pi's add-on decodes the track, or the track's
    // own decoder cannot (see above).
    static std::unique_ptr<SubstituteVideoDecoderHaiku> createIfNeeded(BMediaTrack&, const media_format& decodedFormat);
    // For a track the media kit gave no decoder at all: its chosen add-on
    // failed to set up. The air/OS image of the Raspberry Pi 4 carries the
    // ROCK 5's Rockchip decoder, which sorts first, takes H.264 and fails
    // there, and the media kit does not try the next add-on. Null if no
    // other add-on takes the stream either.
    static std::unique_ptr<SubstituteVideoDecoderHaiku> createForTrackWithoutDecoder(BMediaTrack&, const media_format& decodedFormat);

    // Returns the decoded format, which asks for what `decodedFormat` asked for.
    const media_format& decodedFormat() const { return m_decodedFormat; }
    status_t readFrame(void* buffer, int64* count, media_header*);
    status_t seekToTime(bigtime_t*);
    bigtime_t currentTime() const;
    void codecInfo(media_codec_info*);

private:
    SubstituteVideoDecoderHaiku(BMediaTrack&, const media_format& encodedFormat);
    bool setUpAddOn(const media_format& requested);
    status_t setUp(const media_format& requested);
    status_t GetNextChunk(const void** chunk, size_t* size, media_header*) final;

    BMediaTrack& m_track;
    media_format m_encodedFormat;
    media_format m_decodedFormat;
    std::unique_ptr<AddOnVideoDecoderHaiku> m_addOn;
    double m_frameRate { 0 };
    int64 m_firstFrame { 0 };
    int64 m_framesDecoded { 0 };
};

} // namespace WebCore
