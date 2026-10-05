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

#include "config.h"
#include "AudioFileReader.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"
#include <DataIO.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <cstring>
#include <wtf/Vector.h>

// decodeAudioData(): PCM WAV files are read here; everything else (MP3, AAC,
// Ogg Vorbis and Opus, FLAC...) is decoded by the Media Kit's readers and
// decoders (the ffmpeg add-on).

namespace WebCore {

static uint16_t readLE16(const uint8_t* bytes) { return bytes[0] | (bytes[1] << 8); }
static uint32_t readLE32(const uint8_t* bytes) { return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (uint32_t(bytes[3]) << 24); }

static RefPtr<AudioBus> busFromInterleaved(std::span<const float> samples, unsigned channels, double sampleRate)
{
    if (!channels || sampleRate <= 0)
        return nullptr;
    size_t frames = samples.size() / channels;
    auto bus = AudioBus::create(channels, frames);
    for (unsigned channel = 0; channel < channels; ++channel) {
        auto destination = bus->channel(channel)->mutableSpan();
        for (size_t frame = 0; frame < frames; ++frame)
            destination[frame] = samples[frame * channels + channel];
    }
    bus->setSampleRate(sampleRate);
    return bus;
}

static RefPtr<AudioBus> decodeWave(std::span<const uint8_t> data)
{
    if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4))
        return nullptr;

    unsigned formatTag = 0;
    unsigned channels = 0;
    unsigned bitsPerSample = 0;
    uint32_t sampleRate = 0;
    std::span<const uint8_t> samples;
    size_t offset = 12;
    while (offset + 8 <= data.size()) {
        auto* chunk = data.data() + offset;
        uint32_t size = readLE32(chunk + 4);
        size_t available = std::min<size_t>(size, data.size() - offset - 8);
        if (!memcmp(chunk, "fmt ", 4) && available >= 16) {
            formatTag = readLE16(chunk + 8);
            channels = readLE16(chunk + 10);
            sampleRate = readLE32(chunk + 12);
            bitsPerSample = readLE16(chunk + 22);
            // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes.
            if (formatTag == 0xfffe && available >= 26)
                formatTag = readLE16(chunk + 8 + 24);
        } else if (!memcmp(chunk, "data", 4))
            samples = data.subspan(offset + 8, available);
        offset += 8 + size + (size & 1);
    }
    // 1 is integer PCM, 3 float.
    if (!channels || !sampleRate || samples.empty() || (formatTag != 1 && formatTag != 3))
        return nullptr;

    unsigned bytesPerSample = bitsPerSample / 8;
    if (!bytesPerSample || (formatTag == 3 && bytesPerSample != 4))
        return nullptr;
    size_t count = samples.size() / bytesPerSample;
    Vector<float> floats(count);
    auto* in = samples.data();
    for (size_t i = 0; i < count; ++i, in += bytesPerSample) {
        float value = 0;
        if (formatTag == 3) {
            memcpy(&value, in, 4);
        } else {
            switch (bytesPerSample) {
            case 1:
                value = (int(in[0]) - 128) / 128.0f;
                break;
            case 2:
                value = int16_t(readLE16(in)) / 32768.0f;
                break;
            case 3:
                value = (int32_t(uint32_t(in[0]) << 8 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 24) >> 8) / 8388608.0f;
                break;
            case 4:
                value = int32_t(readLE32(in)) / 2147483648.0f;
                break;
            default:
                return nullptr;
            }
        }
        floats[i] = value;
    }
    return busFromInterleaved(floats.span(), channels, sampleRate);
}

static RefPtr<AudioBus> decodeWithMediaKit(std::span<const uint8_t> data)
{
    BMemoryIO input(data.data(), data.size());
    BMediaFile file(&input);
    if (file.InitCheck() != B_OK)
        return nullptr;

    for (int32 index = 0; index < file.CountTracks(); ++index) {
        BMediaTrack* track = file.TrackAt(index);
        if (!track)
            continue;
        media_format format;
        format.type = B_MEDIA_RAW_AUDIO;
        format.u.raw_audio = media_raw_audio_format::wildcard;
        format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
        format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
        if (track->DecodedFormat(&format) != B_OK || format.type != B_MEDIA_RAW_AUDIO
            || format.u.raw_audio.format != media_raw_audio_format::B_AUDIO_FLOAT) {
            file.ReleaseTrack(track);
            continue;
        }
        unsigned channels = format.u.raw_audio.channel_count;
        double sampleRate = format.u.raw_audio.frame_rate;
        size_t bufferSize = format.u.raw_audio.buffer_size;
        if (!channels || sampleRate <= 0 || !bufferSize) {
            file.ReleaseTrack(track);
            continue;
        }

        Vector<float> samples;
        int64 estimatedFrames = track->CountFrames();
        if (estimatedFrames > 0 && estimatedFrames < (1ll << 31))
            samples.reserveInitialCapacity(estimatedFrames * channels);
        Vector<uint8_t> buffer(bufferSize);
        while (true) {
            int64 frames = 0;
            media_header header;
            status_t status = track->ReadFrames(buffer.mutableSpan().data(), &frames, &header);
            if (frames > 0) {
                size_t count = std::min<size_t>(frames * channels, bufferSize / sizeof(float));
                samples.append(std::span<const float>(reinterpret_cast<const float*>(buffer.span().data()), count));
            }
            if (status != B_OK || frames <= 0)
                break;
        }
        file.ReleaseTrack(track);
        if (samples.isEmpty())
            return nullptr;
        return busFromInterleaved(samples.span(), channels, sampleRate);
    }
    return nullptr;
}

RefPtr<AudioBus> createBusFromInMemoryAudioFile(std::span<const uint8_t> data, bool mixToMono, float sampleRate)
{
    RefPtr bus = decodeWave(data);
    if (!bus)
        bus = decodeWithMediaKit(data);
    if (!bus)
        return nullptr;

    if (sampleRate > 0 && sampleRate != bus->sampleRate())
        return AudioBus::createBySampleRateConverting(*bus, mixToMono, sampleRate);
    if (mixToMono && bus->numberOfChannels() > 1)
        return AudioBus::createByMixingToMono(*bus);
    return bus;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
