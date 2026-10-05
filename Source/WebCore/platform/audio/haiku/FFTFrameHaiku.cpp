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

#if ENABLE(WEB_AUDIO) && PLATFORM(HAIKU)

#include "FFTFrame.h"

#include "VectorMath.h"
#include <array>
#include <bit>
#include <cmath>
#include <wtf/Lock.h>
#include <numbers>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <pffft.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

// Web Audio's FFTs with PFFFT (from the libwebrtc tree), laid out as the
// cross-platform code expects (as vDSP has them on Cocoa): for a frame of N,
// realData()[k] and imagData()[k] are bin k for 0 < k < N/2, realData()[0]
// is the DC component and imagData()[0] the Nyquist one, both real. The
// forward transform is the textbook DFT, the inverse is scaled so that
// inverse(forward(x)) == x.

namespace WebCore {

constexpr unsigned minimumFFTPow2Size = 2;
constexpr unsigned maximumFFTPow2Size = 24;
// PFFFT's real transform takes multiples of 32; smaller ones are done by hand.
constexpr unsigned minimumPFFFTSize = 32;

static Lock setupsLock;

static PFFFT_Setup* setupForSize(unsigned fftSize)
{
    if (fftSize < minimumPFFFTSize)
        return nullptr;
    static std::array<PFFFT_Setup*, maximumFFTPow2Size + 1> setups WTF_GUARDED_BY_LOCK(setupsLock) { };
    unsigned log2Size = std::bit_width(fftSize) - 1;
    Locker locker { setupsLock };
    auto& setup = setups[log2Size];
    // Shared: a setup is read-only once made.
    if (!setup)
        setup = pffft_new_setup(fftSize, PFFFT_REAL);
    return setup;
}

void FFTFrame::allocateTransform()
{
    m_setup = setupForSize(m_FFTSize);
    if (!m_FFTSize)
        return;
    m_buffer = static_cast<float*>(pffft_aligned_malloc(m_FFTSize * sizeof(float)));
    if (m_setup)
        m_work = static_cast<float*>(pffft_aligned_malloc(m_FFTSize * sizeof(float)));
}

FFTFrame::FFTFrame(unsigned fftSize)
    : m_FFTSize(fftSize)
    , m_log2FFTSize(std::bit_width(fftSize) - 1)
    , m_realData(fftSize)
    , m_imagData(fftSize)
{
    ASSERT(1u << m_log2FFTSize == m_FFTSize);
    allocateTransform();
}

FFTFrame::FFTFrame()
    : m_FFTSize(0)
    , m_log2FFTSize(0)
{
}

FFTFrame::FFTFrame(const FFTFrame& frame)
    : m_FFTSize(frame.m_FFTSize)
    , m_log2FFTSize(frame.m_log2FFTSize)
    , m_realData(frame.m_FFTSize)
    , m_imagData(frame.m_FFTSize)
{
    allocateTransform();
    memcpySpan(m_realData.span(), frame.m_realData.span());
    memcpySpan(m_imagData.span(), frame.m_imagData.span());
}

FFTFrame::~FFTFrame()
{
    if (m_buffer)
        pffft_aligned_free(m_buffer);
    if (m_work)
        pffft_aligned_free(m_work);
}

void FFTFrame::initialize()
{
}

void FFTFrame::doFFT(std::span<const float> data)
{
    unsigned size = m_FFTSize;
    unsigned halfSize = size / 2;
    if (!size || data.size() < size)
        return;
    auto realData = m_realData.span();
    auto imagData = m_imagData.span();

    if (m_setup) {
        memcpy(m_buffer, data.data(), size * sizeof(float));
        pffft_transform_ordered(m_setup, m_buffer, m_buffer, m_work, PFFFT_FORWARD);
        realData[0] = m_buffer[0];
        imagData[0] = m_buffer[1];
        for (unsigned k = 1; k < halfSize; ++k) {
            realData[k] = m_buffer[2 * k];
            imagData[k] = m_buffer[2 * k + 1];
        }
        return;
    }

    // A small transform, directly.
    for (unsigned k = 0; k <= halfSize; ++k) {
        double real = 0;
        double imag = 0;
        for (unsigned n = 0; n < size; ++n) {
            double angle = 2 * std::numbers::pi * k * n / size;
            real += data[n] * std::cos(angle);
            imag -= data[n] * std::sin(angle);
        }
        if (!k)
            realData[0] = real;
        else if (k == halfSize)
            imagData[0] = real;
        else {
            realData[k] = real;
            imagData[k] = imag;
        }
    }
}

void FFTFrame::doInverseFFT(std::span<float> data)
{
    unsigned size = m_FFTSize;
    unsigned halfSize = size / 2;
    if (!size || data.size() < size)
        return;
    auto realData = m_realData.span();
    auto imagData = m_imagData.span();

    if (m_setup) {
        m_buffer[0] = realData[0];
        m_buffer[1] = imagData[0];
        for (unsigned k = 1; k < halfSize; ++k) {
            m_buffer[2 * k] = realData[k];
            m_buffer[2 * k + 1] = imagData[k];
        }
        pffft_transform_ordered(m_setup, m_buffer, m_buffer, m_work, PFFFT_BACKWARD);
        memcpy(data.data(), m_buffer, size * sizeof(float));
    } else {
        for (unsigned n = 0; n < size; ++n) {
            double value = realData[0] + ((n & 1) ? -imagData[0] : imagData[0]);
            for (unsigned k = 1; k < halfSize; ++k) {
                double angle = 2 * std::numbers::pi * k * n / size;
                value += 2 * (realData[k] * std::cos(angle) - imagData[k] * std::sin(angle));
            }
            data[n] = value;
        }
    }

    // So that x == inverse(forward(x)).
    VectorMath::multiplyByScalar(data.first(size), 1.0f / size, data);
}

int FFTFrame::minFFTSize()
{
    return 1 << minimumFFTPow2Size;
}

int FFTFrame::maxFFTSize()
{
    return 1 << maximumFFTPow2Size;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(HAIKU)
