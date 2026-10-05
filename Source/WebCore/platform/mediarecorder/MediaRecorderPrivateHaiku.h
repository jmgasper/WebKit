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

#if ENABLE(MEDIA_RECORDER) && PLATFORM(HAIKU) && USE(LIBWEBRTC)

#include "MediaRecorderPrivate.h"
#include "MediaRecorderPrivateOptions.h"

namespace WebCore {

class ContentType;
class HaikuWebMRecorder;

// MediaRecorder on Haiku: WebM with VP8 or VP9 (libvpx) and Opus, encoded on
// a work queue of its own and muxed as it goes (libwebm), all from the
// libwebrtc tree.
class MediaRecorderPrivateHaiku final : public MediaRecorderPrivate {
    WTF_MAKE_TZONE_ALLOCATED(MediaRecorderPrivateHaiku);
public:
    static std::unique_ptr<MediaRecorderPrivateHaiku> create(MediaStreamPrivate&, const MediaRecorderPrivateOptions&);
    static bool isTypeSupported(const ContentType&);

    MediaRecorderPrivateHaiku(MediaStreamPrivate&, const MediaRecorderPrivateOptions&, Ref<HaikuWebMRecorder>&&);
    ~MediaRecorderPrivateHaiku();

private:
    // MediaRecorderPrivate
    void fetchData(FetchDataCallback&&) final;
    String mimeType() const final;
    void startRecording(StartRecordingCallback&&) final;
    void stopRecording(CompletionHandler<void()>&&) final;
    void pauseRecording(CompletionHandler<void()>&&) final;
    void resumeRecording(CompletionHandler<void()>&&) final;

    // RealtimeMediaSource observers, on the sources' threads.
    void videoFrameAvailable(VideoFrame&, VideoFrameTimeMetadata) final;
    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData&, const AudioStreamDescription&, size_t) final;

    const Ref<HaikuWebMRecorder> m_recorder;
    String m_mimeType;
};

} // namespace WebCore

#endif
