/*
 * Copyright (C) 2026 Summit contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#pragma once

// A streaming parser for the fragmented MP4 (ISO BMFF) that Media Source
// Extensions append: an initialization segment (ftyp + moov) followed by
// media segments (moof + mdat pairs, optionally with styp/sidx/emsg). Bytes can
// arrive in pieces of any size. It deliberately depends only on the standard
// library so it can be tested away from Haiku.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace SummitMP4 {

struct TrackInfo {
    enum class Kind { Video, Audio, Other };

    uint32_t id { 0 };
    Kind kind { Kind::Other };
    uint32_t timescale { 0 };
    // Sample entry four-character code: "avc1", "avc3", "mp4a", "Opus", ...
    std::string codec;
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t channels { 0 };
    uint32_t sampleRate { 0 };
    // avcC payload for H.264, AudioSpecificConfig for AAC, dOps for Opus.
    std::vector<uint8_t> codecConfig;
    uint8_t objectTypeIndication { 0 };

    // Defaults from the movie extends box (trex).
    uint32_t defaultSampleDuration { 0 };
    uint32_t defaultSampleSize { 0 };
    uint32_t defaultSampleFlags { 0 };
};

struct InitSegment {
    uint32_t movieTimescale { 0 };
    // In movieTimescale; zero when unknown (typical for live or MSE streams).
    uint64_t duration { 0 };
    std::vector<TrackInfo> tracks;
};

struct Sample {
    uint32_t trackID { 0 };
    // In the track's timescale.
    int64_t decodeTime { 0 };
    int64_t presentationTime { 0 };
    uint32_t duration { 0 };
    bool isSync { false };
    std::vector<uint8_t> data;
};

class Parser {
public:
    std::function<void(InitSegment&&)> onInitSegment;
    std::function<void(Sample&&)> onSample;

    // Returns false once the stream cannot be parsed; the parser then drops
    // everything it holds until reset().
    bool append(const uint8_t*, size_t);
    void reset();

    // Timescale of a track from the most recent initialization segment.
    uint32_t timescaleForTrack(uint32_t trackID) const;

private:
    struct Run {
        uint32_t trackID { 0 };
        uint64_t baseDecodeTime { 0 };
        struct Entry {
            uint64_t offset { 0 }; // Absolute stream offset of the sample data.
            uint32_t size { 0 };
            uint32_t duration { 0 };
            int64_t compositionOffset { 0 };
            bool isSync { false };
        };
        std::vector<Entry> entries;
    };

    bool parseTopLevelBox(const uint8_t*, uint64_t size, const char type[4], uint64_t streamOffset, size_t header);
    bool parseMovie(const uint8_t*, size_t);
    bool parseTrack(const uint8_t*, size_t, TrackInfo&);
    bool parseSampleDescription(const uint8_t*, size_t, TrackInfo&);
    bool parseMovieFragment(const uint8_t*, size_t, uint64_t moofOffset);
    bool parseTrackFragment(const uint8_t*, size_t, uint64_t moofOffset);
    bool emitSamples(uint64_t mdatOffset, uint64_t mdatEnd);
    const TrackInfo* track(uint32_t) const;

    std::vector<uint8_t> m_buffer;
    // Stream offset of m_buffer[0].
    uint64_t m_bufferOffset { 0 };
    bool m_failed { false };
    InitSegment m_init;
    bool m_haveInit { false };
    std::vector<Run> m_pendingRuns;
    // Stream offset of the moof whose samples are pending, or UINT64_MAX.
    uint64_t m_pendingMoofOffset { UINT64_MAX };
};

} // namespace SummitMP4
