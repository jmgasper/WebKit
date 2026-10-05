/*
 *  Copyright (c) 2026 Summit contributors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// WEBRTC_WEBKIT_BUILD change: H.264 encoding on Haiku through the system's
// OpenH264, which is loaded when first needed and may not be installed.

#ifndef MODULES_VIDEO_CODING_CODECS_H264_H264_ENCODER_HAIKU_H_
#define MODULES_VIDEO_CODING_CODECS_H264_H264_ENCODER_HAIKU_H_

#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"

namespace webrtc {

// Whether libopenh264.so.7 loads and is the version whose interface this was
// built with (the API headers are OpenH264 2.4.1's).
bool IsOpenH264EncoderAvailableHaiku();

// libwebrtc's OpenH264 encoder (H264EncoderImpl); null when OpenH264 is not
// available.
std::unique_ptr<VideoEncoder> CreateOpenH264EncoderHaiku(const Environment&, const SdpVideoFormat&);

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_H264_H264_ENCODER_HAIKU_H_
