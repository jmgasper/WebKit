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

#include <WebCore/LibWebRTCProvider.h>

namespace WebCore {

// Video codecs are libwebrtc's own: VP8 and VP9 through libvpx (in the
// libwebrtc tree), as other browsers without a hardware encoder do.
class WEBCORE_EXPORT LibWebRTCProviderHaiku : public LibWebRTCProvider {
public:
    LibWebRTCProviderHaiku() = default;

    std::unique_ptr<webrtc::VideoEncoderFactory> createEncoderFactory() override;
    std::unique_ptr<webrtc::VideoDecoderFactory> createDecoderFactory() override;
};

} // namespace WebCore

#endif // USE(LIBWEBRTC) && PLATFORM(HAIKU)
