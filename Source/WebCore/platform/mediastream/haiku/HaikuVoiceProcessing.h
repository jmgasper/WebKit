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

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU) && USE(LIBWEBRTC)

#include <span>
#include <wtf/Lock.h>
#include <wtf/Vector.h>

namespace webrtc {
class AudioProcessing;
template<typename T> class scoped_refptr;
}

namespace WebCore {

// Echo cancellation, noise suppression and gain control for the microphone
// (libwebrtc's audio processing module, AEC3), as the other browsers do for
// calls: what the page plays (streams and Web Audio) is the echo to remove
// from what the microphone hears. One per web process.
class HaikuVoiceProcessing {
    WTF_MAKE_NONCOPYABLE(HaikuVoiceProcessing);
public:
    static HaikuVoiceProcessing& singleton();
    // SUMMIT_VOICE_PROCESSING=0 turns it off.
    static bool isAvailable();

    // The microphone's samples, interleaved, exactly 10 ms of them; processed
    // in place. Called on the capture thread.
    void processCapture(std::span<float>, unsigned channels, unsigned sampleRate, bool echoCancellation, bool noiseSuppression, bool autoGainControl);

    // What goes to the speakers, interleaved, any length; any thread.
    void analyzeRender(std::span<const float>, unsigned channels, unsigned sampleRate);

private:
    friend class NeverDestroyed<HaikuVoiceProcessing>;
    HaikuVoiceProcessing();
    ~HaikuVoiceProcessing() = delete;

    void configure(bool echoCancellation, bool noiseSuppression, bool autoGainControl);

    webrtc::AudioProcessing* m_processing { nullptr };
    Lock m_configurationLock;
    bool m_echoCancellation { false };
    bool m_noiseSuppression { false };
    bool m_autoGainControl { false };
    bool m_configured { false };

    // Capture side: planes for the float interface.
    Vector<float> m_capturePlanes;

    // Render side: samples wait for a whole 10 ms chunk.
    Lock m_renderLock;
    Vector<float> m_renderPending WTF_GUARDED_BY_LOCK(m_renderLock);
    Vector<float> m_renderPlanes WTF_GUARDED_BY_LOCK(m_renderLock);
    unsigned m_renderChannels WTF_GUARDED_BY_LOCK(m_renderLock) { 0 };
    unsigned m_renderRate WTF_GUARDED_BY_LOCK(m_renderLock) { 0 };
    // When the page last played something (MonotonicTime seconds).
    std::atomic<double> m_lastRender { 0 };
};

} // namespace WebCore

#endif
