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

#include "config.h"
#include "MediaDecoderSelectionHaiku.h"

#include <FindDirectory.h>
#include <MediaFormats.h>
#include <Path.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <image.h>
#include <mutex>
#include <private/media/DecoderPlugin.h>
#include <set>
#include <sys/stat.h>
#include <vector>
#include <wtf/Threading.h>

#if CPU(ARM64)
#include <arm_neon.h>
#endif

namespace WebCore {

namespace {

// Reads the bits of a NAL unit's payload, with its emulation prevention bytes
// (00 00 03) already removed.
class BitReader {
public:
    explicit BitReader(std::vector<uint8_t>&& bytes)
        : m_bytes(std::move(bytes))
    {
    }

    bool failed() const { return m_failed; }

    unsigned bit()
    {
        if (m_position >= m_bytes.size() * 8) {
            m_failed = true;
            return 0;
        }
        unsigned value = (m_bytes[m_position / 8] >> (7 - m_position % 8)) & 1;
        ++m_position;
        return value;
    }

    unsigned bits(unsigned count)
    {
        unsigned value = 0;
        while (count--)
            value = (value << 1) | bit();
        return value;
    }

    // Exp-Golomb, ue(v).
    unsigned unsignedGolomb()
    {
        unsigned zeros = 0;
        while (!bit()) {
            if (m_failed || ++zeros > 31) {
                m_failed = true;
                return 0;
            }
        }
        return (1u << zeros) - 1 + bits(zeros);
    }

    int signedGolomb()
    {
        unsigned value = unsignedGolomb();
        return value & 1 ? static_cast<int>((value + 1) / 2) : -static_cast<int>(value / 2);
    }

private:
    std::vector<uint8_t> m_bytes;
    size_t m_position { 0 };
    bool m_failed { false };
};

void skipScalingList(BitReader& reader, unsigned size)
{
    int lastScale = 8;
    int nextScale = 8;
    for (unsigned i = 0; i < size && !reader.failed(); ++i) {
        if (nextScale)
            nextScale = (lastScale + reader.signedGolomb() + 256) % 256;
        lastScale = nextScale ? nextScale : lastScale;
    }
}

// Section 7.3.2.1.1 of ITU-T H.264, as far as frame_mbs_only_flag.
std::optional<H264SequenceInfoHaiku> sequenceInfoFromSPS(std::span<const uint8_t> nal)
{
    if (nal.size() < 4 || (nal[0] & 0x1f) != 7)
        return std::nullopt;
    std::vector<uint8_t> payload;
    payload.reserve(nal.size());
    unsigned zeros = 0;
    for (size_t i = 1; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        payload.push_back(nal[i]);
    }
    BitReader reader(std::move(payload));
    H264SequenceInfoHaiku info;
    info.profile = reader.bits(8);
    reader.bits(16); // constraint flags, level
    reader.unsignedGolomb(); // seq_parameter_set_id
    static constexpr unsigned highProfiles[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
    if (std::find(std::begin(highProfiles), std::end(highProfiles), info.profile) != std::end(highProfiles)) {
        info.chromaFormat = reader.unsignedGolomb();
        if (info.chromaFormat == 3)
            reader.bit(); // separate_colour_plane_flag
        info.bitDepthLuma = 8 + reader.unsignedGolomb();
        info.bitDepthChroma = 8 + reader.unsignedGolomb();
        reader.bit(); // qpprime_y_zero_transform_bypass_flag
        if (reader.bit()) { // seq_scaling_matrix_present_flag
            for (unsigned i = 0; i < (info.chromaFormat == 3 ? 12u : 8u); ++i) {
                if (reader.bit())
                    skipScalingList(reader, i < 6 ? 16 : 64);
            }
        }
    }
    reader.unsignedGolomb(); // log2_max_frame_num_minus4
    unsigned pictureOrderCountType = reader.unsignedGolomb();
    if (!pictureOrderCountType)
        reader.unsignedGolomb(); // log2_max_pic_order_cnt_lsb_minus4
    else if (pictureOrderCountType == 1) {
        reader.bit(); // delta_pic_order_always_zero_flag
        reader.signedGolomb(); // offset_for_non_ref_pic
        reader.signedGolomb(); // offset_for_top_to_bottom_field
        unsigned cycle = reader.unsignedGolomb();
        if (cycle > 255)
            return std::nullopt;
        for (unsigned i = 0; i < cycle; ++i)
            reader.signedGolomb();
    }
    info.maxReferenceFrames = reader.unsignedGolomb();
    reader.bit(); // gaps_in_frame_num_value_allowed_flag
    reader.unsignedGolomb(); // pic_width_in_mbs_minus1
    reader.unsignedGolomb(); // pic_height_in_map_units_minus1
    info.frameMbsOnly = reader.bit();
    if (reader.failed())
        return std::nullopt;
    return info;
}

unsigned hardwareDecoderReferenceLimit()
{
    // 17 pictures: the references, the one being decoded and one waiting to
    // be shown. Measured on the X399: 15 reference frames play all 300
    // pictures of a clip, 16 play 22.
    static const unsigned limit = [] {
        if (const char* value = std::getenv("SUMMIT_NVDEC_MAX_REFERENCES")) {
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value, &end, 10);
            if (end != value && !*end && parsed <= 32)
                return static_cast<unsigned>(parsed);
        }
        return 15u;
    }();
    return limit;
}

std::mutex s_softwareDecoderLock;
std::optional<media_codec_info> s_softwareDecoder;

// The ffmpeg add-on's codec numbers (AV_CODEC_ID_*), which name the formats
// of the media kit's 'ffmp' family.
constexpr uint32 ffmpegCodecH264 = 27;

// A decoder add-on loaded by name, and what it is known to decode.
struct NamedDecoderAddOn {
    const char* leafName;
    uint32 codec;
    unsigned maxWidth;
    unsigned maxHeight;
    unsigned maxArea;
    BPrivate::media::DecoderPlugin* plugin;
};

// rpi_mmal of air/OS: the VideoCore firmware's decoder of the Raspberry Pi,
// eight-bit 4:2:0 progressive H.264 up to 1920x1088 (either way round).
NamedDecoderAddOn s_namedAddOns[] = {
    { "rpi_mmal", ffmpegCodecH264, 1920, 1920, 1920 * 1088, nullptr },
};

// What dynamic_cast<void*> does, which the engine (built without RTTI) cannot:
// the start of the object a polymorphic subobject belongs to, from the offset
// to top in its virtual table (Itanium C++ ABI). A decoder add-on's plugin
// class derives from DecoderPlugin first, so that is where it starts; the
// offset of DecoderPlugin's virtual base, MediaPlugin, confirms it.
BPrivate::media::DecoderPlugin* decoderPluginOf(BPrivate::media::MediaPlugin* plugin)
{
    if (!plugin)
        return nullptr;
    auto* table = *reinterpret_cast<const ptrdiff_t* const*>(plugin);
    auto* start = reinterpret_cast<char*>(plugin) + table[-2];
    auto* startTable = *reinterpret_cast<const ptrdiff_t* const*>(start);
    if (start + startTable[-3] != reinterpret_cast<char*>(plugin))
        return nullptr;
    return reinterpret_cast<BPrivate::media::DecoderPlugin*>(start);
}

// The firmware's decoder service can stop answering for good: when the
// firmware's memory runs out, or after a picture could not be handed over
// (the VCHIQ driver needs about 3 MB of contiguous memory below 1 GB for a
// 1080p picture, which a full system may not have). Every request then waits
// 5 s. Once a decoder timed out, or took that long to close, the add-on is
// not used again until the system restarts: in this process at once, in the
// others through a marker that names this boot.
std::atomic<bool> s_addOnBroken { false };
// Failed decoders still closing: until they have, whether the firmware still
// answers is not known, and none is set up meanwhile.
std::atomic<unsigned> s_failedDecodersClosing { 0 };
constexpr const char* addOnBrokenMarker = "/tmp/Summit hardware video failed";

long long bootTimeSeconds()
{
    return static_cast<long long>(real_time_clock()) - system_time() / 1000000;
}

bool addOnMarkedBroken()
{
    if (s_addOnBroken || s_failedDecodersClosing)
        return true;
    FILE* marker = std::fopen(addOnBrokenMarker, "r");
    if (!marker)
        return false;
    long long boot = 0;
    bool thisBoot = std::fscanf(marker, "%lld", &boot) == 1 && std::llabs(boot - bootTimeSeconds()) <= 5;
    std::fclose(marker);
    if (thisBoot) {
        s_addOnBroken = true;
        std::fprintf(stderr, "Summit media: the hardware video decoder failed earlier; libavcodec decodes until the system restarts\n");
    }
    return thisBoot;
}

void markAddOnBroken(const char* reason)
{
    if (s_addOnBroken.exchange(true))
        return;
    std::fprintf(stderr, "Summit media: %s; the hardware video decoder is not used again until the system restarts\n", reason);
    if (FILE* marker = std::fopen(addOnBrokenMarker, "w")) {
        std::fprintf(marker, "%lld\n", bootTimeSeconds());
        std::fclose(marker);
    }
}

class NoChunks final : public BPrivate::media::ChunkProvider {
public:
    status_t GetNextChunk(const void**, size_t* size, media_header*) final
    {
        *size = 0;
        return B_LAST_BUFFER_ERROR;
    }
};

// Closes a decoder that failed on a thread of its own: closing waits for the
// firmware, which may not answer.
void closeFailedDecoder(std::unique_ptr<BPrivate::media::Decoder>&& decoder)
{
    if (!decoder)
        return;
    decoder->SetChunkProvider(new NoChunks);
    ++s_failedDecodersClosing;
    Thread::create("Summit decoder close"_s, [decoder = WTF::move(decoder)]() mutable {
        bigtime_t started = system_time();
        decoder = nullptr;
        if (system_time() - started > 4000000)
            markAddOnBroken("the hardware video decoder took seconds to close");
        --s_failedDecodersClosing;
    })->detach();
}

bool hardwareVideoAllowed()
{
    static const bool allowed = [] {
        const char* value = std::getenv("SUMMIT_HARDWARE_VIDEO");
        return !value || std::strcmp(value, "0");
    }();
    return allowed;
}

// The add-on that decodes `codec`, loaded the first time one is asked for.
// The first add-on directory that has one wins, as in the media kit.
NamedDecoderAddOn* namedAddOnFor(uint32 codec)
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (!hardwareVideoAllowed())
            return;
        char** paths = nullptr;
        size_t count = 0;
        if (find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/plugins", &paths, &count) != B_OK)
            return;
        for (auto& addOn : s_namedAddOns) {
            for (size_t i = 0; i < count && !addOn.plugin; ++i) {
                BPath path(paths[i], addOn.leafName);
                struct stat info;
                if (path.InitCheck() != B_OK || stat(path.Path(), &info) || !S_ISREG(info.st_mode))
                    continue;
                image_id image = load_add_on(path.Path());
                if (image < 0)
                    continue;
                BPrivate::media::MediaPlugin* (*instantiate)() = nullptr;
                if (get_image_symbol(image, "instantiate_plugin", B_SYMBOL_TYPE_TEXT, reinterpret_cast<void**>(&instantiate)) != B_OK || !instantiate) {
                    unload_add_on(image);
                    continue;
                }
                addOn.plugin = decoderPluginOf(instantiate());
                if (!addOn.plugin) {
                    std::fprintf(stderr, "Summit media: %s is not a decoder add-on\n", path.Path());
                    break;
                }
            }
        }
        free(paths);
    });
    for (auto& addOn : s_namedAddOns) {
        if (addOn.plugin && addOn.codec == codec)
            return &addOn;
    }
    return nullptr;
}

std::optional<uint32> ffmpegCodecOf(const media_format& format)
{
    media_format_description description;
    BMediaFormats formats;
    if (formats.InitCheck() != B_OK || formats.GetCodeFor(format, B_MISC_FORMAT_FAMILY, &description) != B_OK
        || description.u.misc.file_format != 'ffmp')
        return std::nullopt;
    return description.u.misc.codec;
}

// The add-on for this stream, if one is installed and the stream is of the
// kind it decodes (eight-bit 4:2:0 progressive H.264 within its size).
NamedDecoderAddOn* namedAddOnForStream(const media_format& encoded, std::span<const uint8_t> codecConfig)
{
    if (encoded.type != B_MEDIA_ENCODED_VIDEO)
        return nullptr;
    auto codec = ffmpegCodecOf(encoded);
    if (!codec)
        return nullptr;
    auto* addOn = namedAddOnFor(*codec);
    if (!addOn || addOnMarkedBroken())
        return nullptr;
    const auto& display = encoded.u.encoded_video.output.display;
    if (!display.line_width || !display.line_count || display.line_width > addOn->maxWidth
        || display.line_count > addOn->maxHeight || display.line_width * display.line_count > addOn->maxArea)
        return nullptr;
    if (*codec == ffmpegCodecH264) {
        auto info = h264SequenceInfo(codecConfig);
        if (!info || info->chromaFormat != 1 || info->bitDepthLuma != 8 || info->bitDepthChroma != 8 || !info->frameMbsOnly)
            return nullptr;
    }
    return addOn;
}

// Not among Haiku's colour spaces: three planes, luma and then Cb and Cr with
// half the rows of half the length (the Raspberry Pi add-on's).
constexpr color_space colorSpaceI420 = static_cast<color_space>(0x49343230); // 'I420'

// Limited-range YCbCr to RGB in sixths of 64 (Q6): BT.709 for pictures of
// 720 lines and more, BT.601 below, which is what streams that say nothing
// else mean in practice.
struct YCbCrToRGB {
    int16_t y;
    int16_t crToR;
    int16_t cbToG;
    int16_t crToG;
    int16_t cbToB;
};
constexpr YCbCrToRGB bt601 { 75, 102, 25, 52, 129 };
constexpr YCbCrToRGB bt709 { 75, 115, 14, 34, 135 };

inline uint8_t clampQ6(int value)
{
    value = (value + 32) >> 6;
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

void convertRowI420(const uint8_t* luma, const uint8_t* cb, const uint8_t* cr, uint8_t* bgra, unsigned width, const YCbCrToRGB& matrix)
{
    unsigned x = 0;
#if CPU(ARM64)
    // Sixteen pixels a step: the luma term and the chroma terms in 16 bits,
    // added with saturation (only results above 255 saturate, and those are
    // clamped to it anyway), then shifted and narrowed with rounding. The
    // pixels are interleaved with zips and stored as four vectors: built in
    // a uint8x16x4_t for st4, GCC 13 sends the channels through the stack.
    const uint8x8_t chromaZero = vdup_n_u8(128);
    const uint8x16_t lumaZero = vdupq_n_u8(16);
    const uint8x16_t alpha = vdupq_n_u8(255);
    for (; x + 16 <= width; x += 16) {
        uint8x16_t y = vld1q_u8(luma + x);
        int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(cb + x / 2), chromaZero));
        int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(cr + x / 2), chromaZero));
        int16x8_t r = vmulq_n_s16(v, matrix.crToR);
        int16x8_t g = vmlaq_n_s16(vmulq_n_s16(u, matrix.cbToG), v, matrix.crToG);
        int16x8_t b = vmulq_n_s16(u, matrix.cbToB);
        int16x8_t yLow = vmulq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(y), vget_low_u8(lumaZero))), matrix.y);
        int16x8_t yHigh = vmulq_n_s16(vreinterpretq_s16_u16(vsubl_high_u8(y, lumaZero)), matrix.y);
        uint8x16_t blue = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(yLow, vzip1q_s16(b, b)), 6), vqrshrun_n_s16(vqaddq_s16(yHigh, vzip2q_s16(b, b)), 6));
        uint8x16_t green = vcombine_u8(vqrshrun_n_s16(vqsubq_s16(yLow, vzip1q_s16(g, g)), 6), vqrshrun_n_s16(vqsubq_s16(yHigh, vzip2q_s16(g, g)), 6));
        uint8x16_t red = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(yLow, vzip1q_s16(r, r)), 6), vqrshrun_n_s16(vqaddq_s16(yHigh, vzip2q_s16(r, r)), 6));
        uint16x8_t blueGreenLow = vreinterpretq_u16_u8(vzip1q_u8(blue, green));
        uint16x8_t blueGreenHigh = vreinterpretq_u16_u8(vzip2q_u8(blue, green));
        uint16x8_t redAlphaLow = vreinterpretq_u16_u8(vzip1q_u8(red, alpha));
        uint16x8_t redAlphaHigh = vreinterpretq_u16_u8(vzip2q_u8(red, alpha));
        uint8_t* out = bgra + x * 4;
        vst1q_u8(out, vreinterpretq_u8_u16(vzip1q_u16(blueGreenLow, redAlphaLow)));
        vst1q_u8(out + 16, vreinterpretq_u8_u16(vzip2q_u16(blueGreenLow, redAlphaLow)));
        vst1q_u8(out + 32, vreinterpretq_u8_u16(vzip1q_u16(blueGreenHigh, redAlphaHigh)));
        vst1q_u8(out + 48, vreinterpretq_u8_u16(vzip2q_u16(blueGreenHigh, redAlphaHigh)));
    }
#endif
    for (; x < width; ++x) {
        int y = (luma[x] - 16) * matrix.y;
        int u = cb[x / 2] - 128;
        int v = cr[x / 2] - 128;
        bgra[x * 4] = clampQ6(y + matrix.cbToB * u);
        bgra[x * 4 + 1] = clampQ6(y - matrix.cbToG * u - matrix.crToG * v);
        bgra[x * 4 + 2] = clampQ6(y + matrix.crToR * v);
        bgra[x * 4 + 3] = 255;
    }
}

} // namespace

std::optional<H264SequenceInfoHaiku> h264SequenceInfo(std::span<const uint8_t> avcC)
{
    if (avcC.size() < 8 || avcC[0] != 1)
        return std::nullopt;
    unsigned sequenceParameterSets = avcC[5] & 0x1f;
    if (!sequenceParameterSets)
        return std::nullopt;
    size_t length = (size_t(avcC[6]) << 8) | avcC[7];
    if (8 + length > avcC.size())
        return std::nullopt;
    return sequenceInfoFromSPS(avcC.subspan(8, length));
}

std::optional<unsigned> h264MaxReferenceFrames(std::span<const uint8_t> avcC)
{
    auto info = h264SequenceInfo(avcC);
    return info ? std::optional { info->maxReferenceFrames } : std::nullopt;
}

bool hardwareDecoderCannotDecodeHaiku(const media_codec_info& decoder, std::span<const uint8_t> avcC)
{
    if (std::strcmp(decoder.short_name, "nvdec h264"))
        return false;
    auto references = h264MaxReferenceFrames(avcC);
    return references && *references > hardwareDecoderReferenceLimit();
}

status_t setSoftwareDecoderHaiku(BMediaDecoder& decoder, const media_format& format, const void* info, size_t infoSize, media_codec_info* chosen)
{
    auto accepts = [&](const media_codec_info& candidate) {
        media_format input = format;
        if (decoder.SetTo(&candidate) != B_OK || decoder.SetInputFormat(&input, info, infoSize) != B_OK)
            return false;
        media_codec_info name = { };
        if (decoder.GetDecoderInfo(&name) != B_OK || !std::strncmp(name.short_name, "nvdec", 5))
            return false;
        if (chosen)
            *chosen = name;
        return true;
    };

    {
        std::lock_guard guard(s_softwareDecoderLock);
        if (s_softwareDecoder && accepts(*s_softwareDecoder))
            return B_OK;
    }
    // BMediaDecoder::SetTo(const media_codec_info*) finds the add-on by the
    // identifier Haiku gives it in its list of encoders, and libavcodec's
    // add-on is the one that both encodes and decodes.
    int32 cookie = 0;
    media_codec_info candidate;
    std::set<int32> tried;
    while (get_next_encoder(&cookie, &candidate) == B_OK) {
        if (!tried.insert(candidate.id).second)
            continue;
        if (accepts(candidate)) {
            std::lock_guard guard(s_softwareDecoderLock);
            s_softwareDecoder = candidate;
            return B_OK;
        }
    }
    return B_ENTRY_NOT_FOUND;
}

class AddOnVideoDecoderHaiku::Forwarder final : public BPrivate::media::ChunkProvider {
public:
    explicit Forwarder(AddOnVideoDecoderHaiku& decoder)
        : m_decoder(decoder)
    {
    }
    status_t GetNextChunk(const void** chunk, size_t* size, media_header* header) final { return m_decoder.m_source(chunk, size, header); }

private:
    AddOnVideoDecoderHaiku& m_decoder;
};

bool AddOnVideoDecoderHaiku::canDecode(const media_format& encoded, std::span<const uint8_t> codecConfig)
{
    return namedAddOnForStream(encoded, codecConfig);
}

std::unique_ptr<AddOnVideoDecoderHaiku> AddOnVideoDecoderHaiku::create(const media_format& encoded, std::span<const uint8_t> codecConfig, ChunkSource&& source)
{
    auto* addOn = namedAddOnForStream(encoded, codecConfig);
    if (!addOn)
        return nullptr;
    auto* decoder = addOn->plugin->NewDecoder(0);
    if (!decoder)
        return nullptr;
    std::unique_ptr<AddOnVideoDecoderHaiku> result(new AddOnVideoDecoderHaiku(decoder, WTF::move(source)));
    decoder->GetCodecInfo(&result->m_codecInfo);
    media_format input = encoded;
    status_t status = decoder->Setup(&input, codecConfig.data(), codecConfig.size());
    media_format output;
    if (status == B_OK) {
        output.type = B_MEDIA_RAW_VIDEO;
        output.u.raw_video = media_raw_video_format::wildcard;
        output.u.raw_video.display.format = colorSpaceI420;
        // Decodes the first picture: it tells how large they are.
        status = decoder->NegotiateOutputFormat(&output);
    }
    const auto& display = output.u.raw_video.display;
    if (status == B_OK && (output.type != B_MEDIA_RAW_VIDEO || display.format != colorSpaceI420 || !display.line_width
        || !display.line_count || display.bytes_per_row < display.line_width))
        status = B_MEDIA_BAD_FORMAT;
    // The source stopped (a seek): nothing is wrong with the decoder.
    if (status == B_CANCELED)
        return nullptr;
    if (status != B_OK) {
        std::fprintf(stderr, "Summit media: %s did not take the stream (%s)\n", result->m_codecInfo.pretty_name, strerror(status));
        if (status == B_TIMED_OUT)
            markAddOnBroken("the hardware video decoder did not answer");
        closeFailedDecoder(std::exchange(result->m_decoder, nullptr));
        return nullptr;
    }
    result->m_width = display.line_width;
    result->m_height = display.line_count;
    result->m_planesRowBytes = display.bytes_per_row;
    result->m_planes.grow(result->layout().chromaOffset(3));
    return result;
}

AddOnVideoDecoderHaiku::AddOnVideoDecoderHaiku(BPrivate::media::Decoder* decoder, ChunkSource&& source)
    : m_source(WTF::move(source))
    , m_decoder(decoder)
{
    m_decoder->SetChunkProvider(new Forwarder(*this));
}

AddOnVideoDecoderHaiku::~AddOnVideoDecoderHaiku() = default;

status_t AddOnVideoDecoderHaiku::decodePlanes(void* planes, int64* count, media_header* header, media_decode_info* info)
{
    return m_decoder->Decode(planes, count, header, info);
}

status_t AddOnVideoDecoderHaiku::decode(void* bgra, size_t rowBytes, int64* count, media_header* header, media_decode_info* info)
{
    status_t status = decodePlanes(m_planes.mutableSpan().data(), count, header, info);
    if (status != B_OK)
        return status;
    // A picture the caller will not show (it is seeking) is not converted;
    // the add-on has not filled the planes for it either.
    if (info && info->time_to_decode < 0 && header->start_time < -info->time_to_decode)
        return B_OK;
    convertToBGRA(m_planes.span().data(), layout(), static_cast<uint8_t*>(bgra), rowBytes);
    return B_OK;
}

void AddOnVideoDecoderHaiku::convertToBGRA(const uint8_t* planes, const I420Layout& layout, uint8_t* bgra, size_t rowBytes)
{
    const size_t chromaRowBytes = layout.lumaRowBytes / 2;
    const uint8_t* cb = planes + layout.chromaOffset(1);
    const uint8_t* cr = planes + layout.chromaOffset(2);
    const auto& matrix = layout.bt709() ? bt709 : bt601;
    for (unsigned row = 0; row < layout.height; ++row) {
        convertRowI420(planes + row * layout.lumaRowBytes, cb + (row / 2) * chromaRowBytes, cr + (row / 2) * chromaRowBytes,
            bgra + row * rowBytes, layout.width, matrix);
    }
}

status_t AddOnVideoDecoderHaiku::seekedTo(int64 frame, bigtime_t time)
{
    return m_decoder->SeekedTo(frame, time);
}

std::unique_ptr<SubstituteVideoDecoderHaiku> SubstituteVideoDecoderHaiku::createIfNeeded(BMediaTrack& track, const media_format& decodedFormat)
{
    media_format encoded;
    media_codec_info native = { };
    if (track.EncodedFormat(&encoded) != B_OK || track.GetCodecInfo(&native) != B_OK)
        return nullptr;
    auto* meta = static_cast<const uint8_t*>(encoded.MetaData());
    std::span<const uint8_t> codecConfig { meta, meta ? static_cast<size_t>(encoded.MetaDataSize()) : 0 };
    const bool addOn = AddOnVideoDecoderHaiku::canDecode(encoded, codecConfig);
    if (!addOn && (!meta || !hardwareDecoderCannotDecodeHaiku(native, codecConfig)))
        return nullptr;
    // libavcodec learns the picture format by decoding the first chunk it is
    // given, and a track taken again from its file carries on from wherever
    // the last one stopped: start it from the beginning. Callers seek it to
    // the time they want afterwards.
    bigtime_t start = 0;
    track.SeekToTime(&start);
    std::unique_ptr<SubstituteVideoDecoderHaiku> decoder(new SubstituteVideoDecoderHaiku(track, encoded));
    if (addOn && decoder->setUpAddOn(decodedFormat)) {
        std::fprintf(stderr, "Summit media: video decoded by %s instead of %s\n", decoder->m_addOn->codecInfo().pretty_name, native.pretty_name);
        return decoder;
    }
    if (!meta || !hardwareDecoderCannotDecodeHaiku(native, codecConfig)) {
        // The add-on refused the stream after all; the track's own decoder
        // starts it again from the beginning.
        start = 0;
        track.SeekToTime(&start);
        return nullptr;
    }
    if (decoder->setUp(decodedFormat) != B_OK) {
        std::fprintf(stderr, "Summit media: %s cannot decode this H.264 stream and no other decoder took it\n", native.pretty_name);
        return nullptr;
    }
    media_codec_info chosen = { };
    decoder->GetDecoderInfo(&chosen);
    std::fprintf(stderr, "Summit media: H.264 with %u reference frames decoded by %s instead of %s\n",
        h264MaxReferenceFrames(codecConfig).value_or(0), chosen.pretty_name, native.pretty_name);
    return decoder;
}

std::unique_ptr<SubstituteVideoDecoderHaiku> SubstituteVideoDecoderHaiku::createForTrackWithoutDecoder(BMediaTrack& track, const media_format& decodedFormat)
{
    media_format encoded;
    if (track.EncodedFormat(&encoded) != B_OK || encoded.type != B_MEDIA_ENCODED_VIDEO)
        return nullptr;
    bigtime_t start = 0;
    track.SeekToTime(&start);
    std::unique_ptr<SubstituteVideoDecoderHaiku> decoder(new SubstituteVideoDecoderHaiku(track, encoded));
    if (decoder->setUpAddOn(decodedFormat)) {
        std::fprintf(stderr, "Summit media: the decoder the media kit chose failed; video decoded by %s\n", decoder->m_addOn->codecInfo().pretty_name);
        return decoder;
    }
    start = 0;
    track.SeekToTime(&start);
    if (decoder->setUp(decodedFormat) != B_OK) {
        std::fprintf(stderr, "Summit media: no decoder add-on takes this video track\n");
        return nullptr;
    }
    media_codec_info chosen = { };
    decoder->GetDecoderInfo(&chosen);
    std::fprintf(stderr, "Summit media: the decoder the media kit chose failed; video decoded by %s\n", chosen.pretty_name);
    return decoder;
}

SubstituteVideoDecoderHaiku::SubstituteVideoDecoderHaiku(BMediaTrack& track, const media_format& encodedFormat)
    : m_track(track)
    , m_encodedFormat(encodedFormat)
{
    double rate = encodedFormat.u.encoded_video.output.field_rate;
    if (rate > 0 && std::isfinite(rate))
        m_frameRate = rate;
    m_firstFrame = track.CurrentFrame();
}

bool SubstituteVideoDecoderHaiku::setUpAddOn(const media_format& requested)
{
    if (requested.type != B_MEDIA_RAW_VIDEO || (requested.u.raw_video.display.format != B_RGB32 && requested.u.raw_video.display.format != 0))
        return false;
    auto* meta = static_cast<const uint8_t*>(m_encodedFormat.MetaData());
    std::span<const uint8_t> codecConfig { meta, meta ? static_cast<size_t>(m_encodedFormat.MetaDataSize()) : 0 };
    m_addOn = AddOnVideoDecoderHaiku::create(m_encodedFormat, codecConfig, [this](const void** chunk, size_t* size, media_header* header) {
        return GetNextChunk(chunk, size, header);
    });
    if (!m_addOn)
        return false;
    media_format output = requested;
    auto& raw = output.u.raw_video;
    raw.display.format = B_RGB32;
    raw.display.line_width = m_addOn->width();
    raw.display.line_count = m_addOn->height();
    raw.display.bytes_per_row = m_addOn->width() * 4;
    raw.display.pixel_offset = 0;
    raw.display.line_offset = 0;
    raw.first_active = 0;
    raw.last_active = m_addOn->height() - 1;
    raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
    if (!raw.pixel_width_aspect || !raw.pixel_height_aspect) {
        raw.pixel_width_aspect = m_encodedFormat.u.encoded_video.output.pixel_width_aspect;
        raw.pixel_height_aspect = m_encodedFormat.u.encoded_video.output.pixel_height_aspect;
    }
    if (!(raw.field_rate > 0))
        raw.field_rate = m_encodedFormat.u.encoded_video.output.field_rate;
    m_decodedFormat = output;
    return true;
}

status_t SubstituteVideoDecoderHaiku::setUp(const media_format& requested)
{
    status_t status = setSoftwareDecoderHaiku(*this, m_encodedFormat, m_encodedFormat.MetaData(), m_encodedFormat.MetaDataSize());
    if (status != B_OK)
        return status;
    media_format output = requested;
    status = SetOutputFormat(&output);
    if (status == B_OK)
        m_decodedFormat = output;
    return status;
}

status_t SubstituteVideoDecoderHaiku::GetNextChunk(const void** chunk, size_t* size, media_header* header)
{
    char* buffer = nullptr;
    int32 length = 0;
    status_t status = m_track.ReadChunk(&buffer, &length, header);
    *chunk = buffer;
    *size = status == B_OK ? static_cast<size_t>(length) : 0;
    return status;
}

status_t SubstituteVideoDecoderHaiku::readFrame(void* buffer, int64* count, media_header* header)
{
    media_header ignored;
    media_decode_info info;
    status_t status = m_addOn
        ? m_addOn->decode(buffer, m_decodedFormat.u.raw_video.display.bytes_per_row, count, header ? header : &ignored, &info)
        : Decode(buffer, count, header ? header : &ignored, &info);
    if (status == B_OK)
        m_framesDecoded += std::max<int64>(*count, 1);
    return status;
}

status_t SubstituteVideoDecoderHaiku::seekToTime(bigtime_t* time)
{
    status_t status = m_track.SeekToTime(time);
    if (status != B_OK)
        return status;
    m_firstFrame = m_track.CurrentFrame();
    m_framesDecoded = 0;
    // Nothing from before the seek may come out of the decoder: the add-on
    // forgets what it has, libavcodec's is made anew.
    if (m_addOn)
        return m_addOn->seekedTo(m_firstFrame, *time);
    return setUp(m_decodedFormat);
}

bigtime_t SubstituteVideoDecoderHaiku::currentTime() const
{
    if (!(m_frameRate > 0))
        return m_track.CurrentTime();
    return static_cast<bigtime_t>((m_firstFrame + m_framesDecoded) * 1000000.0 / m_frameRate);
}

void SubstituteVideoDecoderHaiku::codecInfo(media_codec_info* info)
{
    if (m_addOn) {
        *info = m_addOn->codecInfo();
        return;
    }
    GetDecoderInfo(info);
}

} // namespace WebCore
