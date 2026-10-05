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
#include "PlatformMediaEngineConfigurationFactoryHaiku.h"

#if PLATFORM(HAIKU)

#include "ContentType.h"
#include "MediaPlayer.h"
#include "PlatformMediaCapabilitiesDecodingInfo.h"
#include "PlatformMediaDecodingConfiguration.h"
#include <wtf/Function.h>

namespace WebCore {

static bool engineSupports(PlatformMediaDecodingType type, const String& contentType)
{
    MediaEngineSupportParameters parameters;
    parameters.platformType = type;
    parameters.type = ContentType(contentType);
    return MediaPlayer::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

void createMediaPlayerDecodingConfigurationHaiku(PlatformMediaDecodingConfiguration&& configuration, Function<void(PlatformMediaCapabilitiesDecodingInfo&&)>&& callback)
{
    bool supported = true;
    bool hardware = false;
    if (auto& video = configuration.video) {
        supported &= engineSupports(configuration.type, video->contentType);
        // Frames are presented as 8-bit sRGB without alpha.
        if (video->alphaChannel.value_or(false) || video->hdrMetadataType
            || (video->transferFunction && *video->transferFunction != PlatformMediaCapabilitiesTransferFunction::SRGB))
            supported = false;
        // H.264 goes to the NVDEC add-on where one is installed.
        for (auto& codec : ContentType(video->contentType).codecs()) {
            if (codec.startsWith("avc1"_s) || codec.startsWith("avc3"_s))
                hardware = true;
        }
    }
    if (auto& audio = configuration.audio) {
        supported &= engineSupports(configuration.type, audio->contentType);
        if (audio->spatialRendering.value_or(false))
            supported = false;
    }

    PlatformMediaCapabilitiesDecodingInfo info;
    info.supported = supported;
    info.smooth = supported;
    info.powerEfficient = supported && hardware;
    info.configuration = WTF::move(configuration);
    callback(WTF::move(info));
}

}

#endif
