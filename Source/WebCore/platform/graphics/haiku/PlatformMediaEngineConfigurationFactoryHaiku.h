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

#include <wtf/Forward.h>

namespace WebCore {

struct PlatformMediaCapabilitiesDecodingInfo;
struct PlatformMediaDecodingConfiguration;

// navigator.mediaCapabilities.decodingInfo() for the Haiku media engines. Without
// a factory WebCore answers "unsupported" for every configuration, and players
// such as YouTube's then refuse formats the engines can play.
void createMediaPlayerDecodingConfigurationHaiku(PlatformMediaDecodingConfiguration&&, Function<void(PlatformMediaCapabilitiesDecodingInfo&&)>&&);

}

#endif
