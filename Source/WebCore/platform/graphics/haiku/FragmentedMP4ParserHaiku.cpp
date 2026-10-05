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

#include "FragmentedMP4ParserHaiku.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>

namespace SummitMP4 {

namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

bool isType(const char* type, const char* expected) { return !std::memcmp(type, expected, 4); }

// Calls visit(type, payload, payloadSize) for each complete box in [data, data + size).
// Returns false if a box header is malformed or visit returns false.
template<typename Visitor>
bool forEachBox(const uint8_t* data, size_t size, Visitor&& visit)
{
    size_t at = 0;
    while (at + 8 <= size) {
        uint64_t boxSize = be32(data + at);
        const char* type = reinterpret_cast<const char*>(data + at + 4);
        size_t header = 8;
        if (boxSize == 1) {
            if (at + 16 > size)
                return false;
            boxSize = be64(data + at + 8);
            header = 16;
        } else if (!boxSize)
            boxSize = size - at;
        if (boxSize < header || boxSize > size - at)
            return false;
        if (!visit(type, data + at + header, size_t(boxSize - header)))
            return false;
        at += size_t(boxSize);
    }
    return true;
}

// ISO 14496-1 descriptor length: up to four bytes, seven bits each.
bool readDescriptorLength(const uint8_t* data, size_t size, size_t& at, size_t& length)
{
    length = 0;
    for (int i = 0; i < 4; ++i) {
        if (at >= size)
            return false;
        uint8_t byte = data[at++];
        length = length << 7 | (byte & 0x7f);
        if (!(byte & 0x80))
            return true;
    }
    return true;
}

void parseElementaryStreamDescriptor(const uint8_t* data, size_t size, TrackInfo& info)
{
    // esds is a full box: version and flags come first.
    if (size < 4)
        return;
    size_t at = 4;
    while (at < size) {
        uint8_t tag = data[at++];
        size_t length;
        if (!readDescriptorLength(data, size, at, length) || at + length > size)
            return;
        size_t end = at + length;
        if (tag == 0x03) {
            // ES_Descriptor: ES_ID, flags, then nested descriptors.
            if (at + 3 > end)
                return;
            uint8_t flags = data[at + 2];
            at += 3;
            if (flags & 0x80)
                at += 2;
            if (flags & 0x40) {
                if (at >= end)
                    return;
                at += 1 + data[at];
            }
            if (flags & 0x20)
                at += 2;
            continue;
        }
        if (tag == 0x04) {
            // DecoderConfigDescriptor, then DecoderSpecificInfo inside it.
            if (at + 13 > end)
                return;
            info.objectTypeIndication = data[at];
            at += 13;
            continue;
        }
        if (tag == 0x05) {
            info.codecConfig.assign(data + at, data + end);
            return;
        }
        at = end;
    }
}

} // namespace

bool Parser::append(const uint8_t* data, size_t size)
{
    if (m_failed)
        return false;
    m_buffer.insert(m_buffer.end(), data, data + size);

    size_t at = 0;
    uint64_t keepFrom = m_bufferOffset;
    while (at + 8 <= m_buffer.size()) {
        uint64_t boxSize = be32(m_buffer.data() + at);
        char type[4];
        std::memcpy(type, m_buffer.data() + at + 4, 4);
        size_t header = 8;
        if (boxSize == 1) {
            if (at + 16 > m_buffer.size())
                break;
            boxSize = be64(m_buffer.data() + at + 8);
            header = 16;
        }
        if (boxSize < header) {
            m_failed = true;
            break;
        }
        if (boxSize > m_buffer.size() - at)
            break; // Wait for the rest of the box.

        uint64_t streamOffset = m_bufferOffset + at;
        if (!parseTopLevelBox(m_buffer.data() + at + header, boxSize - header, type, streamOffset, header)) {
            m_failed = true;
            break;
        }
        at += size_t(boxSize);
        keepFrom = m_pendingMoofOffset != UINT64_MAX ? m_pendingMoofOffset : m_bufferOffset + at;
    }

    if (m_failed) {
        m_buffer.clear();
        m_pendingRuns.clear();
        m_pendingMoofOffset = UINT64_MAX;
        return false;
    }

    // Keep only what a pending moof still refers to and the incomplete box.
    size_t discard = size_t(std::min<uint64_t>(keepFrom - m_bufferOffset, m_buffer.size()));
    if (discard) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + discard);
        m_bufferOffset += discard;
    }
    return true;
}

void Parser::reset()
{
    m_buffer.clear();
    m_bufferOffset = 0;
    m_failed = false;
    m_pendingRuns.clear();
    m_pendingMoofOffset = UINT64_MAX;
}

uint32_t Parser::timescaleForTrack(uint32_t trackID) const
{
    auto* info = track(trackID);
    return info ? info->timescale : 0;
}

const TrackInfo* Parser::track(uint32_t trackID) const
{
    for (auto& info : m_init.tracks) {
        if (info.id == trackID)
            return &info;
    }
    return nullptr;
}

bool Parser::parseTopLevelBox(const uint8_t* payload, uint64_t size, const char type[4], uint64_t streamOffset, size_t header)
{
    if (isType(type, "moov"))
        return parseMovie(payload, size_t(size));
    if (isType(type, "moof")) {
        m_pendingRuns.clear();
        m_pendingMoofOffset = streamOffset;
        if (!m_haveInit)
            return false;
        return parseMovieFragment(payload, size_t(size), streamOffset);
    }
    if (isType(type, "mdat")) {
        if (m_pendingMoofOffset == UINT64_MAX)
            return true; // Data nobody pointed at.
        bool ok = emitSamples(streamOffset + header, streamOffset + header + size);
        m_pendingRuns.clear();
        m_pendingMoofOffset = UINT64_MAX;
        return ok;
    }
    // ftyp, styp, sidx, emsg, free, skip, prft, uuid: nothing to play.
    return true;
}

bool Parser::parseMovie(const uint8_t* data, size_t size)
{
    InitSegment init;
    std::map<uint32_t, TrackInfo> defaults;
    uint64_t fragmentDuration = 0;
    bool ok = forEachBox(data, size, [&](const char* type, const uint8_t* payload, size_t length) {
        if (isType(type, "mvhd")) {
            if (length < 4)
                return false;
            if (payload[0] == 1) {
                if (length < 32)
                    return false;
                init.movieTimescale = be32(payload + 20);
                init.duration = be64(payload + 24);
            } else {
                if (length < 20)
                    return false;
                init.movieTimescale = be32(payload + 12);
                init.duration = be32(payload + 16);
                if (init.duration == UINT32_MAX)
                    init.duration = 0;
            }
            return true;
        }
        if (isType(type, "trak")) {
            TrackInfo info;
            if (!parseTrack(payload, length, info))
                return false;
            if (info.id)
                init.tracks.push_back(std::move(info));
            return true;
        }
        if (isType(type, "mvex")) {
            return forEachBox(payload, length, [&](const char* childType, const uint8_t* child, size_t childLength) {
                if (isType(childType, "trex") && childLength >= 24) {
                    TrackInfo& info = defaults[be32(child + 4)];
                    info.defaultSampleDuration = be32(child + 12);
                    info.defaultSampleSize = be32(child + 16);
                    info.defaultSampleFlags = be32(child + 20);
                } else if (isType(childType, "mehd") && childLength >= 8)
                    fragmentDuration = child[0] == 1 && childLength >= 12 ? be64(child + 4) : be32(child + 4);
                return true;
            });
        }
        return true;
    });
    if (!ok)
        return false;
    for (auto& info : init.tracks) {
        auto found = defaults.find(info.id);
        if (found == defaults.end())
            continue;
        info.defaultSampleDuration = found->second.defaultSampleDuration;
        info.defaultSampleSize = found->second.defaultSampleSize;
        info.defaultSampleFlags = found->second.defaultSampleFlags;
    }
    if (!init.duration)
        init.duration = fragmentDuration;

    m_init = init;
    m_haveInit = true;
    if (onInitSegment)
        onInitSegment(std::move(init));
    return true;
}

bool Parser::parseTrack(const uint8_t* data, size_t size, TrackInfo& info)
{
    return forEachBox(data, size, [&](const char* type, const uint8_t* payload, size_t length) {
        if (isType(type, "tkhd")) {
            size_t idOffset = payload[0] == 1 ? 20 : 12;
            if (length < idOffset + 4)
                return false;
            info.id = be32(payload + idOffset);
            return true;
        }
        if (!isType(type, "mdia"))
            return true;
        return forEachBox(payload, length, [&](const char* mediaType, const uint8_t* media, size_t mediaLength) {
            if (isType(mediaType, "mdhd")) {
                size_t scaleOffset = media[0] == 1 ? 20 : 12;
                if (mediaLength < scaleOffset + 4)
                    return false;
                info.timescale = be32(media + scaleOffset);
            } else if (isType(mediaType, "hdlr")) {
                if (mediaLength < 12)
                    return false;
                const char* handler = reinterpret_cast<const char*>(media + 8);
                if (isType(handler, "vide"))
                    info.kind = TrackInfo::Kind::Video;
                else if (isType(handler, "soun"))
                    info.kind = TrackInfo::Kind::Audio;
            } else if (isType(mediaType, "minf")) {
                return forEachBox(media, mediaLength, [&](const char* infoType, const uint8_t* mediaInfo, size_t infoLength) {
                    if (!isType(infoType, "stbl"))
                        return true;
                    return forEachBox(mediaInfo, infoLength, [&](const char* tableType, const uint8_t* table, size_t tableLength) {
                        if (isType(tableType, "stsd"))
                            return parseSampleDescription(table, tableLength, info);
                        return true;
                    });
                });
            }
            return true;
        });
    });
}

bool Parser::parseSampleDescription(const uint8_t* data, size_t size, TrackInfo& info)
{
    // Full box header, entry count, then the first sample entry.
    if (size < 16)
        return false;
    const uint8_t* entry = data + 8;
    size_t entrySize = be32(entry);
    if (entrySize < 8 || entrySize > size - 8)
        return false;
    info.codec.assign(reinterpret_cast<const char*>(entry + 4), 4);
    const uint8_t* payload = entry + 8;
    size_t length = entrySize - 8;

    if (info.kind == TrackInfo::Kind::Video) {
        if (length < 78)
            return true;
        info.width = be16(payload + 24);
        info.height = be16(payload + 26);
        return forEachBox(payload + 78, length - 78, [&](const char* type, const uint8_t* child, size_t childLength) {
            if (isType(type, "avcC") || isType(type, "hvcC") || isType(type, "vpcC") || isType(type, "av1C"))
                info.codecConfig.assign(child, child + childLength);
            return true;
        });
    }
    if (info.kind == TrackInfo::Kind::Audio) {
        if (length < 28)
            return true;
        uint16_t version = be16(payload + 8);
        info.channels = be16(payload + 16);
        info.sampleRate = be32(payload + 24) >> 16;
        size_t childrenAt = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
        if (childrenAt > length)
            return true;
        return forEachBox(payload + childrenAt, length - childrenAt, [&](const char* type, const uint8_t* child, size_t childLength) {
            if (isType(type, "esds"))
                parseElementaryStreamDescriptor(child, childLength, info);
            else if (isType(type, "dOps"))
                info.codecConfig.assign(child, child + childLength);
            return true;
        });
    }
    return true;
}

bool Parser::parseMovieFragment(const uint8_t* data, size_t size, uint64_t moofOffset)
{
    return forEachBox(data, size, [&](const char* type, const uint8_t* payload, size_t length) {
        if (isType(type, "traf"))
            return parseTrackFragment(payload, length, moofOffset);
        return true;
    });
}

bool Parser::parseTrackFragment(const uint8_t* data, size_t size, uint64_t moofOffset)
{
    uint32_t trackID = 0;
    uint64_t base = moofOffset;
    uint32_t defaultDuration = 0, defaultSize = 0, defaultFlags = 0;
    bool haveDefaults = false;
    std::optional<uint64_t> baseDecodeTime;
    const TrackInfo* info = nullptr;
    uint64_t nextDataOffset = 0;
    bool firstRun = true;
    uint64_t decodeTime = 0;
    size_t firstRunIndex = m_pendingRuns.size();

    bool ok = forEachBox(data, size, [&](const char* type, const uint8_t* payload, size_t length) {
        if (isType(type, "tfhd")) {
            if (length < 8)
                return false;
            uint32_t flags = be32(payload) & 0xffffff;
            trackID = be32(payload + 4);
            info = track(trackID);
            if (!info)
                return true;
            defaultDuration = info->defaultSampleDuration;
            defaultSize = info->defaultSampleSize;
            defaultFlags = info->defaultSampleFlags;
            size_t at = 8;
            auto need = [&](size_t bytes) { return at + bytes <= length; };
            if (flags & 0x1) {
                if (!need(8))
                    return false;
                base = be64(payload + at);
                at += 8;
            }
            if (flags & 0x2)
                at += 4;
            if (flags & 0x8) {
                if (!need(4))
                    return false;
                defaultDuration = be32(payload + at);
                at += 4;
            }
            if (flags & 0x10) {
                if (!need(4))
                    return false;
                defaultSize = be32(payload + at);
                at += 4;
            }
            if (flags & 0x20) {
                if (!need(4))
                    return false;
                defaultFlags = be32(payload + at);
                at += 4;
            }
            haveDefaults = true;
            return true;
        }
        if (!info)
            return true;
        if (isType(type, "tfdt")) {
            if (length < 8)
                return false;
            baseDecodeTime = payload[0] == 1 && length >= 12 ? be64(payload + 4) : be32(payload + 4);
            return true;
        }
        if (!isType(type, "trun"))
            return true;
        if (!haveDefaults || length < 8)
            return false;

        uint8_t version = payload[0];
        uint32_t flags = be32(payload) & 0xffffff;
        uint32_t count = be32(payload + 4);
        size_t at = 8;
        uint64_t dataOffset = firstRun ? base : nextDataOffset;
        if (flags & 0x1) {
            if (at + 4 > length)
                return false;
            dataOffset = base + int64_t(int32_t(be32(payload + at)));
            at += 4;
        }
        std::optional<uint32_t> firstSampleFlags;
        if (flags & 0x4) {
            if (at + 4 > length)
                return false;
            firstSampleFlags = be32(payload + at);
            at += 4;
        }
        size_t perSample = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) + ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
        if (perSample && count > (length - at) / perSample)
            return false;

        if (firstRun)
            decodeTime = baseDecodeTime.value_or(0);
        Run run;
        run.trackID = trackID;
        run.baseDecodeTime = decodeTime;
        run.entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Run::Entry entry;
            entry.duration = defaultDuration;
            entry.size = defaultSize;
            uint32_t sampleFlags = i == 0 && firstSampleFlags ? *firstSampleFlags : defaultFlags;
            if (flags & 0x100) {
                entry.duration = be32(payload + at);
                at += 4;
            }
            if (flags & 0x200) {
                entry.size = be32(payload + at);
                at += 4;
            }
            if (flags & 0x400) {
                sampleFlags = be32(payload + at);
                at += 4;
            }
            if (flags & 0x800) {
                uint32_t value = be32(payload + at);
                entry.compositionOffset = version ? int64_t(int32_t(value)) : int64_t(value);
                at += 4;
            }
            entry.offset = dataOffset;
            dataOffset += entry.size;
            entry.isSync = info->kind == TrackInfo::Kind::Audio || !((sampleFlags >> 16) & 1);
            run.entries.push_back(entry);
        }
        nextDataOffset = dataOffset;
        firstRun = false;
        for (auto& entry : run.entries)
            decodeTime += entry.duration;
        m_pendingRuns.push_back(std::move(run));
        return true;
    });
    if (!ok)
        m_pendingRuns.resize(firstRunIndex);
    return ok;
}

bool Parser::emitSamples(uint64_t mdatOffset, uint64_t mdatEnd)
{
    uint64_t bufferEnd = m_bufferOffset + m_buffer.size();
    for (auto& run : m_pendingRuns) {
        int64_t decodeTime = int64_t(run.baseDecodeTime);
        for (auto& entry : run.entries) {
            if (entry.offset < m_bufferOffset || entry.offset + entry.size > bufferEnd
                || entry.offset < mdatOffset || entry.offset + entry.size > mdatEnd)
                return false;
            Sample sample;
            sample.trackID = run.trackID;
            sample.decodeTime = decodeTime;
            sample.presentationTime = decodeTime + entry.compositionOffset;
            sample.duration = entry.duration;
            sample.isSync = entry.isSync;
            const uint8_t* bytes = m_buffer.data() + (entry.offset - m_bufferOffset);
            sample.data.assign(bytes, bytes + entry.size);
            decodeTime += entry.duration;
            if (onSample)
                onSample(std::move(sample));
        }
    }
    return true;
}

} // namespace SummitMP4
