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

#if USE(LIBWEBRTC) && PLATFORM(HAIKU)

#include <memory>

namespace webrtc {
class VideoDecoderFactory;
class VideoEncoderFactory;
}

namespace WebCore {

// libwebrtc's decoders (VP8, VP9) and, when the system has FFmpeg, H.264.
std::unique_ptr<webrtc::VideoDecoderFactory> createVideoDecoderFactoryHaiku();

// libwebrtc's encoders (VP8, VP9) and, when the system has OpenH264, H.264.
std::unique_ptr<webrtc::VideoEncoderFactory> createVideoEncoderFactoryHaiku();

} // namespace WebCore

#endif // USE(LIBWEBRTC) && PLATFORM(HAIKU)
