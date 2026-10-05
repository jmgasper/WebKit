/*
 *  Copyright (c) 2026 Summit contributors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// WEBRTC_WEBKIT_BUILD change: see h264_encoder_haiku.h.

#include "modules/video_coding/codecs/h264/h264_encoder_haiku.h"

#include <dlfcn.h>

#include "modules/video_coding/codecs/h264/h264_encoder_impl.h"
#include "rtc_base/logging.h"

// The two functions h264_encoder_impl.cc calls by name are defined below and
// stay inside this library.
#pragma GCC visibility push(hidden)
#include "third_party/openh264/src/codec/api/wels/codec_api.h"
#pragma GCC visibility pop
#include "third_party/openh264/src/codec/api/wels/codec_ver.h"

namespace webrtc {

namespace {

struct OpenH264Library {
  int (*create_encoder)(ISVCEncoder**);
  void (*destroy_encoder)(ISVCEncoder*);
};

const OpenH264Library* LoadOpenH264() {
  static const OpenH264Library* library = []() -> const OpenH264Library* {
    // The soname changes when the library's interface does.
    void* handle = dlopen("libopenh264.so.7", RTLD_NOW | RTLD_LOCAL);
    if (!handle)
      return nullptr;
    auto get_version = reinterpret_cast<void (*)(OpenH264Version*)>(
        dlsym(handle, "WelsGetCodecVersionEx"));
    auto create_encoder = reinterpret_cast<int (*)(ISVCEncoder**)>(
        dlsym(handle, "WelsCreateSVCEncoder"));
    auto destroy_encoder = reinterpret_cast<void (*)(ISVCEncoder*)>(
        dlsym(handle, "WelsDestroySVCEncoder"));
    if (!get_version || !create_encoder || !destroy_encoder) {
      dlclose(handle);
      return nullptr;
    }
    // The parameter structures are passed by pointer: they have to be the
    // ones of the headers this was built with.
    OpenH264Version version = {};
    get_version(&version);
    if (version.uMajor != OPENH264_MAJOR || version.uMinor != OPENH264_MINOR) {
      RTC_LOG(LS_WARNING) << "OpenH264 " << version.uMajor << "."
                          << version.uMinor << " is not the expected "
                          << OPENH264_MAJOR << "." << OPENH264_MINOR;
      dlclose(handle);
      return nullptr;
    }
    return new OpenH264Library{create_encoder, destroy_encoder};
  }();
  return library;
}

}  // namespace

bool IsOpenH264EncoderAvailableHaiku() {
  return !!LoadOpenH264();
}

std::unique_ptr<VideoEncoder> CreateOpenH264EncoderHaiku(
    const Environment& env,
    const SdpVideoFormat& format) {
  if (!LoadOpenH264())
    return nullptr;
  return std::make_unique<H264EncoderImpl>(env,
                                           H264EncoderSettings::Parse(format));
}

}  // namespace webrtc

extern "C" int WelsCreateSVCEncoder(ISVCEncoder** encoder) {
  auto* library = webrtc::LoadOpenH264();
  if (!library) {
    *encoder = nullptr;
    return 1;
  }
  return library->create_encoder(encoder);
}

extern "C" void WelsDestroySVCEncoder(ISVCEncoder* encoder) {
  if (auto* library = webrtc::LoadOpenH264())
    library->destroy_encoder(encoder);
}
