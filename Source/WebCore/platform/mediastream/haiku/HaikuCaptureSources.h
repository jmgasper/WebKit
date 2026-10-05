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

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "CaptureDevice.h"
#include "CaptureDeviceManager.h"
#include "DisplayCaptureManager.h"
#include "RealtimeMediaSource.h"
#include "RealtimeMediaSourceCapabilities.h"
#include "RealtimeMediaSourceFactory.h"
#include "RealtimeMediaSourceSettings.h"
#include "RealtimeVideoCaptureSource.h"
#include <MediaDefs.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MediaTime.h>
#include <wtf/Threading.h>

namespace WebCore {

class HaikuCaptureConnection;

// Cameras (video) or microphones (audio) the Media Kit has: the default one
// first.
class HaikuCaptureDeviceManager final : public CaptureDeviceManager {
public:
    static HaikuCaptureDeviceManager& cameras();
    static HaikuCaptureDeviceManager& microphones();

    const Vector<CaptureDevice>& captureDevices() final;
    void computeCaptureDevices(CompletionHandler<void()>&&) final;

private:
    friend class NeverDestroyed<HaikuCaptureDeviceManager>;
    explicit HaikuCaptureDeviceManager(CaptureDevice::DeviceType type)
        : m_type(type)
    {
    }
    void refresh();

    CaptureDevice::DeviceType m_type;
    Vector<CaptureDevice> m_devices;
    bool m_computed { false };
};

// The screens app_server draws and the windows on the current workspace, for
// getDisplayMedia(): the browser asks which one the person wants to share, so
// the list is part of the request.
class HaikuDisplayCaptureDeviceManager final : public DisplayCaptureManager {
public:
    static HaikuDisplayCaptureDeviceManager& singleton();

    const Vector<CaptureDevice>& captureDevices() final;
    void computeCaptureDevices(CompletionHandler<void()>&&) final;
    bool requiresCaptureDevicesEnumeration() const final { return true; }

private:
    friend class NeverDestroyed<HaikuDisplayCaptureDeviceManager>;
    HaikuDisplayCaptureDeviceManager() = default;
    void refresh();

    Vector<CaptureDevice> m_devices;
};

// A camera, through a consumer node of ours connected to it. It sends B_RGB32
// pictures of the size asked for; they go out as I420.
class HaikuVideoCaptureSource final : public RealtimeVideoCaptureSource {
public:
    static CaptureSourceOrError create(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>);
    ~HaikuVideoCaptureSource();

    const RealtimeMediaSourceCapabilities& capabilities() final;
    const RealtimeMediaSourceSettings& settings() final;

private:
    HaikuVideoCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, std::optional<PageIdentifier>);

    bool isCaptureSource() const final { return true; }
    CaptureDevice::DeviceType deviceType() const final { return CaptureDevice::DeviceType::Camera; }
    void startProducingData() final;
    void stopProducingData() final;
    void settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>) final;
    void generatePresets() final;
    bool canResizeVideoFrames() const final { return true; }
    void applyFrameRateAndZoomWithPreset(double, double, std::optional<VideoPreset>&&) final;
    void configurationChanged() final;

    void connect();
    void disconnect();
    void pictureReceived(const void*, size_t, const media_format&);

    std::unique_ptr<HaikuCaptureConnection> m_connection;
    IntSize m_connectedSize;
    std::optional<RealtimeMediaSourceCapabilities> m_capabilities;
    std::optional<RealtimeMediaSourceSettings> m_currentSettings;
    // Pictures are dropped to keep to the frame rate asked for.
    MonotonicTime m_lastPicture;
};

// A screen, or a window's part of it, read from app_server
// (BScreen::ReadBitmap) on a thread of its own at the frame rate asked for. A
// window is followed as it moves and resizes; what covers it is captured
// with it (app_server keeps no picture of a window's hidden parts).
class HaikuDisplayCaptureSource final : public RealtimeVideoCaptureSource {
public:
    static CaptureSourceOrError create(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>);
    ~HaikuDisplayCaptureSource();

    const RealtimeMediaSourceCapabilities& capabilities() final;
    const RealtimeMediaSourceSettings& settings() final;

private:
    HaikuDisplayCaptureSource(const CaptureDevice&, int32_t screenId, int32_t windowToken, IntSize, MediaDeviceHashSalts&&, std::optional<PageIdentifier>);

    bool isCaptureSource() const final { return true; }
    CaptureDevice::DeviceType deviceType() const final { return m_windowToken >= 0 ? CaptureDevice::DeviceType::Window : CaptureDevice::DeviceType::Screen; }
    void startProducingData() final;
    void stopProducingData() final;
    void generatePresets() final;
    bool canResizeVideoFrames() const final { return true; }
    void configurationChanged() final;
    void settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>) final;

    void captureLoop();

    const int32_t m_screenId;
    // The window's app_server token, or -1 for the whole screen.
    const int32_t m_windowToken;
    const IntSize m_screenSize;
    RefPtr<Thread> m_thread;
    Lock m_lock;
    Condition m_condition;
    bool m_running WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::atomic<double> m_captureFrameRate { 15 };
    std::atomic<int> m_outputWidth { 0 };
    std::atomic<int> m_outputHeight { 0 };
    std::optional<RealtimeMediaSourceCapabilities> m_capabilities;
    std::optional<RealtimeMediaSourceSettings> m_currentSettings;
};

// A microphone (or a sound card's input), through a consumer node of ours.
// Samples go out as interleaved float (HaikuAudioData).
// Destroyed on the main run loop: the last reference can be dropped on the
// capture node's thread, which cannot stop and disconnect its own node.
class HaikuAudioCaptureSource final : public RealtimeMediaSource, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<HaikuAudioCaptureSource, WTF::DestructionThread::MainRunLoop> {
public:
    static CaptureSourceOrError create(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>);
    ~HaikuAudioCaptureSource();

    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

    const RealtimeMediaSourceCapabilities& capabilities() final;
    const RealtimeMediaSourceSettings& settings() final;

private:
    HaikuAudioCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, std::optional<PageIdentifier>);

    bool isCaptureSource() const final { return true; }
    CaptureDevice::DeviceType deviceType() const final { return CaptureDevice::DeviceType::Microphone; }
    void startProducingData() final;
    void stopProducingData() final;
    void settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>) final;
    void configurationChanged() final;

    void samplesReceived(const void*, size_t, const media_format&);

    void sendSamples(std::span<float>, unsigned channels, double rate);

    std::unique_ptr<HaikuCaptureConnection> m_connection;
    Vector<float> m_samples;
    // Samples waiting for a whole 10 ms chunk when voice processing is on.
    Vector<float> m_pending;
    std::atomic<bool> m_voiceProcessing { false };
    uint64_t m_framesSent { 0 };
    MediaTime m_baseTime { MediaTime::invalidTime() };
    std::atomic<float> m_gain { 1 };
    std::optional<RealtimeMediaSourceCapabilities> m_capabilities;
    std::optional<RealtimeMediaSourceSettings> m_currentSettings;
};

class HaikuAudioCaptureFactory final : public AudioCaptureFactory {
public:
    static HaikuAudioCaptureFactory& singleton();
private:
    CaptureSourceOrError createAudioCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>) final;
    CaptureDeviceManager& audioCaptureDeviceManager() final { return HaikuCaptureDeviceManager::microphones(); }
    const Vector<CaptureDevice>& speakerDevices() const final { return m_speakers; }
    Vector<CaptureDevice> m_speakers;
};

class HaikuVideoCaptureFactory final : public VideoCaptureFactory {
public:
    static HaikuVideoCaptureFactory& singleton();
private:
    CaptureSourceOrError createVideoCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>) final;
    CaptureDeviceManager& videoCaptureDeviceManager() final { return HaikuCaptureDeviceManager::cameras(); }
};

class HaikuDisplayCaptureFactory final : public DisplayCaptureFactory {
public:
    static HaikuDisplayCaptureFactory& singleton();
private:
    CaptureSourceOrError createDisplayCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>) final;
    DisplayCaptureManager& displayCaptureDeviceManager() final { return HaikuDisplayCaptureDeviceManager::singleton(); }
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
