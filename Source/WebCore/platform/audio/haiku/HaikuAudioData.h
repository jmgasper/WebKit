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

#if PLATFORM(HAIKU)

#include "AudioStreamDescription.h"
#include "PlatformAudioData.h"
#include <span>
#include <wtf/TypeCasts.h>

namespace WebCore {

// Audio samples as the Haiku port passes them between capture, WebRTC,
// players and recorders: interleaved 32-bit float. The samples belong to the
// caller and are valid during the call that hands them over; an observer
// that keeps them copies them.
class HaikuAudioData final : public PlatformAudioData {
public:
    HaikuAudioData(std::span<const float> samples, unsigned channelCount)
        : m_samples(samples)
        , m_channelCount(channelCount)
    {
    }

    std::span<const float> samples() const { return m_samples; }
    unsigned channelCount() const { return m_channelCount; }
    size_t frameCount() const { return m_channelCount ? m_samples.size() / m_channelCount : 0; }

private:
    Kind kind() const final { return Kind::HaikuAudioData; }

    std::span<const float> m_samples;
    unsigned m_channelCount { 0 };
};

class HaikuAudioStreamDescription final : public AudioStreamDescription {
public:
    HaikuAudioStreamDescription(double sampleRate, unsigned channelCount)
        : m_sampleRate(sampleRate)
        , m_channelCount(channelCount)
    {
    }

    const PlatformDescription& platformDescription() const final
    {
        m_platformDescription = { PlatformDescription::HaikuAudioStreamDescription, nullptr };
        return m_platformDescription;
    }
    PCMFormat format() const final { return Float32; }
    double sampleRate() const final { return m_sampleRate; }
    bool isInterleaved() const final { return true; }
    bool isSignedInteger() const final { return false; }
    bool isFloat() const final { return true; }
    bool isNativeEndian() const final { return true; }
    uint32_t numberOfInterleavedChannels() const final { return m_channelCount; }
    uint32_t numberOfChannelStreams() const final { return 1; }
    uint32_t numberOfChannels() const final { return m_channelCount; }
    uint32_t sampleWordSize() const final { return sizeof(float); }

    bool operator==(const HaikuAudioStreamDescription& other) const { return m_sampleRate == other.m_sampleRate && m_channelCount == other.m_channelCount; }

private:
    double m_sampleRate { 0 };
    unsigned m_channelCount { 0 };
    mutable PlatformDescription m_platformDescription;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::HaikuAudioData)
static bool isType(const WebCore::PlatformAudioData& data) { return data.kind() == WebCore::PlatformAudioData::Kind::HaikuAudioData; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // PLATFORM(HAIKU)
