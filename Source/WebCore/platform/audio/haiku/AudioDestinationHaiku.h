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

#if ENABLE(WEB_AUDIO)

#include "AudioDestinationResampler.h"
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Vector.h>

class BSoundPlayer;
struct media_raw_audio_format;

namespace WebCore {

// Web Audio's output: a BSoundPlayer at the system mixer's rate. Its buffers
// are filled from the render quanta AudioDestinationResampler queues.
class AudioDestinationHaiku final : public AudioDestinationResampler, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<AudioDestinationHaiku, WTF::DestructionThread::Main> {
public:
    explicit AudioDestinationHaiku(const CreationOptions&);
    ~AudioDestinationHaiku();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

private:
    void startRendering(CompletionHandler<void(bool)>&&) final;
    void stopRendering(CompletionHandler<void(bool)>&&) final;
    MediaTime outputLatency() const final;

    static void playBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format&);
    void fill(float* output, size_t frames, unsigned channels);

    BSoundPlayer* m_output { nullptr };
    Vector<float> m_planes;
    double m_framesPlayed { 0 };
};

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
