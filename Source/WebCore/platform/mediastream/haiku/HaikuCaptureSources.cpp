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
#include "HaikuCaptureSources.h"

#if ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)

#include "HaikuAudioData.h"
#include "HaikuCaptureNode.h"
#include "HaikuVoiceProcessing.h"
#include "MediaConstraints.h"
#include "RealtimeMediaSourceCenter.h"
#include "VideoFrameHaiku.h"
#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <Rect.h>
#include <cmath>
#include <cstdlib>
#include <interface/Screen.h>
#include <private/interface/WindowInfo.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <libyuv.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

static MediaTime currentMediaTime()
{
    return MediaTime::createWithDouble(MonotonicTime::now().secondsSinceEpoch().value());
}

// Device lists.

HaikuCaptureDeviceManager& HaikuCaptureDeviceManager::cameras()
{
    static NeverDestroyed<HaikuCaptureDeviceManager> manager(CaptureDevice::DeviceType::Camera);
    return manager;
}

HaikuCaptureDeviceManager& HaikuCaptureDeviceManager::microphones()
{
    static NeverDestroyed<HaikuCaptureDeviceManager> manager(CaptureDevice::DeviceType::Microphone);
    return manager;
}

void HaikuCaptureDeviceManager::refresh()
{
    Vector<CaptureDevice> devices;
    auto type = m_type == CaptureDevice::DeviceType::Camera ? B_MEDIA_RAW_VIDEO : B_MEDIA_RAW_AUDIO;
    for (auto& input : haikuMediaInputs(type)) {
        CaptureDevice device(input.persistentId, m_type, input.label);
        device.setEnabled(true);
        device.setIsDefault(input.isDefault);
        devices.append(WTF::move(device));
    }
    m_computed = true;
    if (haveDevicesChanged(m_devices, devices)) {
        m_devices = WTF::move(devices);
        deviceChanged();
    }
}

const Vector<CaptureDevice>& HaikuCaptureDeviceManager::captureDevices()
{
    if (!m_computed)
        refresh();
    return m_devices;
}

void HaikuCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&& callback)
{
    refresh();
    callback();
}

HaikuDisplayCaptureDeviceManager& HaikuDisplayCaptureDeviceManager::singleton()
{
    static NeverDestroyed<HaikuDisplayCaptureDeviceManager> manager;
    return manager;
}

void HaikuDisplayCaptureDeviceManager::refresh()
{
    m_devices.clear();
    BScreen screen(B_MAIN_SCREEN_ID);
    unsigned index = 0;
    while (screen.IsValid()) {
        ++index;
        BRect frame = screen.Frame();
        auto label = makeString("Screen "_s, index, " ("_s, int(frame.IntegerWidth()) + 1, " x "_s, int(frame.IntegerHeight()) + 1, ')');
        CaptureDevice device(makeString("haiku-screen:"_s, screen.ID().id), CaptureDevice::DeviceType::Screen, label);
        device.setEnabled(true);
        device.setIsDefault(index == 1);
        m_devices.append(WTF::move(device));
        if (screen.SetToNext() != B_OK)
            break;
    }

    // The windows on the current workspace, front to back: those that are
    // shown, not minimized, of some size, and not the desktop or Deskbar.
    int32* tokens = nullptr;
    int32 count = 0;
    if (BPrivate::get_window_order(current_workspace(), &tokens, &count) != B_OK || !tokens)
        return;
    for (int32 index = 0; index < count; ++index) {
        client_window_info* info = get_window_info(tokens[index]);
        if (!info)
            continue;
        int width = info->window_right - info->window_left + 1;
        int height = info->window_bottom - info->window_top + 1;
        String title = String::fromUTF8(info->name);
        bool shareable = info->show_hide_level <= 0 && !info->is_mini && width >= 64 && height >= 48
            && !title.isEmpty() && title != "Deskbar"_s && title != "Desktop"_s;
        if (shareable) {
            CaptureDevice device(makeString("haiku-window:"_s, info->server_token), CaptureDevice::DeviceType::Window, makeString("Window: "_s, title));
            device.setEnabled(true);
            m_devices.append(WTF::move(device));
        }
        free(info);
    }
    free(tokens);
}

const Vector<CaptureDevice>& HaikuDisplayCaptureDeviceManager::captureDevices()
{
    refresh();
    return m_devices;
}

void HaikuDisplayCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&& callback)
{
    refresh();
    callback();
}

// Factories.

HaikuAudioCaptureFactory& HaikuAudioCaptureFactory::singleton()
{
    static NeverDestroyed<HaikuAudioCaptureFactory> factory;
    return factory;
}

CaptureSourceOrError HaikuAudioCaptureFactory::createAudioCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    return HaikuAudioCaptureSource::create(device, WTF::move(hashSalts), constraints, pageIdentifier);
}

HaikuVideoCaptureFactory& HaikuVideoCaptureFactory::singleton()
{
    static NeverDestroyed<HaikuVideoCaptureFactory> factory;
    return factory;
}

CaptureSourceOrError HaikuVideoCaptureFactory::createVideoCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    return HaikuVideoCaptureSource::create(device, WTF::move(hashSalts), constraints, pageIdentifier);
}

HaikuDisplayCaptureFactory& HaikuDisplayCaptureFactory::singleton()
{
    static NeverDestroyed<HaikuDisplayCaptureFactory> factory;
    return factory;
}

CaptureSourceOrError HaikuDisplayCaptureFactory::createDisplayCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    return HaikuDisplayCaptureSource::create(device, WTF::move(hashSalts), constraints, pageIdentifier);
}

// Camera.

CaptureSourceOrError HaikuVideoCaptureSource::create(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    auto source = adoptRef(*new HaikuVideoCaptureSource(device, WTF::move(hashSalts), pageIdentifier));
    if (constraints) {
        if (auto error = source->applyConstraints(*constraints))
            return CaptureSourceOrError(CaptureSourceError { error->invalidConstraint });
    }
    return CaptureSourceOrError(WTF::move(source));
}

HaikuVideoCaptureSource::HaikuVideoCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, std::optional<PageIdentifier> pageIdentifier)
    : RealtimeVideoCaptureSource(device, WTF::move(hashSalts), pageIdentifier)
{
    // What a page gets without asking: 640x480 at 30, as other browsers.
    setIntrinsicSize({ 640, 480 }, false);
    setFrameRate(30);
}

HaikuVideoCaptureSource::~HaikuVideoCaptureSource()
{
    disconnect();
}

void HaikuVideoCaptureSource::generatePresets()
{
    // The camera's add-on scales or crops to any size (the next larger mode of
    // the camera); these are the sizes pages ask for.
    static constexpr std::array<IntSize, 11> sizes { {
        { 160, 120 }, { 320, 180 }, { 320, 240 }, { 424, 240 }, { 640, 360 }, { 640, 480 },
        { 848, 480 }, { 960, 540 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 },
    } };
    Vector<VideoPreset> presets;
    for (auto& size : sizes)
        presets.append(VideoPreset { { size, { { 1, 30 } } } });
    setSupportedPresets(WTF::move(presets));
}

void HaikuVideoCaptureSource::applyFrameRateAndZoomWithPreset(double, double, std::optional<VideoPreset>&& preset)
{
    if (preset)
        setIntrinsicSize(preset->size());
}

const RealtimeMediaSourceCapabilities& HaikuVideoCaptureSource::capabilities()
{
    if (!m_capabilities) {
        RealtimeMediaSourceCapabilities capabilities(settings().supportedConstraints());
        capabilities.setDeviceId(hashedId());
        updateCapabilities(capabilities);
        capabilities.addFacingMode(VideoFacingMode::Unknown);
        m_capabilities = WTF::move(capabilities);
    }
    return *m_capabilities;
}

const RealtimeMediaSourceSettings& HaikuVideoCaptureSource::settings()
{
    if (!m_currentSettings) {
        RealtimeMediaSourceSettings settings;
        settings.setDeviceId(hashedId());
        RealtimeMediaSourceSupportedConstraints supportedConstraints;
        supportedConstraints.setSupportsDeviceId(true);
        supportedConstraints.setSupportsFacingMode(true);
        supportedConstraints.setSupportsWidth(true);
        supportedConstraints.setSupportsHeight(true);
        supportedConstraints.setSupportsAspectRatio(true);
        supportedConstraints.setSupportsFrameRate(true);
        settings.setSupportedConstraints(supportedConstraints);
        m_currentSettings = WTF::move(settings);
    }
    m_currentSettings->setWidth(size().width());
    m_currentSettings->setHeight(size().height());
    m_currentSettings->setFrameRate(frameRate());
    m_currentSettings->setFacingMode(facingMode());
    return *m_currentSettings;
}

void HaikuVideoCaptureSource::configurationChanged()
{
    m_currentSettings = std::nullopt;
    m_capabilities = std::nullopt;
    RealtimeMediaSource::configurationChanged();
}

void HaikuVideoCaptureSource::settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag> flags)
{
    m_currentSettings = std::nullopt;
    if (!flags.containsAny({ RealtimeMediaSourceSettings::Flag::Width, RealtimeMediaSourceSettings::Flag::Height }))
        return;
    // A new size is a new connection.
    if (m_connection && size() != m_connectedSize) {
        disconnect();
        connect();
    }
}

void HaikuVideoCaptureSource::connect()
{
    auto input = haikuMediaInputWithId(B_MEDIA_RAW_VIDEO, persistentID());
    if (!input) {
        captureFailed();
        return;
    }

    auto size = this->size();
    if (size.isEmpty())
        size = { 640, 480 };
    media_format format;
    format.type = B_MEDIA_RAW_VIDEO;
    format.u.raw_video = media_raw_video_format::wildcard;
    format.u.raw_video.display.format = B_RGB32;
    format.u.raw_video.display.line_width = size.width();
    format.u.raw_video.display.line_count = size.height();

    String error;
    m_connection = HaikuCaptureConnection::connect(input->node, format, [weakThis = ThreadSafeWeakPtr { *this }](const void* data, size_t bytes, const media_format& format, bigtime_t) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->pictureReceived(data, bytes, format);
    }, error);
    if (!m_connection) {
        fprintf(stderr, "Summit: camera \"%s\": %s\n", name().utf8().data(), error.utf8().data());
        captureFailed();
        return;
    }
    m_connectedSize = size;
}

void HaikuVideoCaptureSource::disconnect()
{
    m_connection = nullptr;
}

void HaikuVideoCaptureSource::startProducingData()
{
    m_lastPicture = { };
    connect();
}

void HaikuVideoCaptureSource::stopProducingData()
{
    disconnect();
}

void HaikuVideoCaptureSource::pictureReceived(const void* data, size_t bytes, const media_format& format)
{
    if (!isProducingData() || muted())
        return;

    // Keep to the frame rate asked for.
    auto now = MonotonicTime::now();
    double rate = frameRate();
    if (rate > 0 && rate < 29 && now - m_lastPicture < Seconds(0.9 / rate))
        return;
    m_lastPicture = now;

    auto& video = format.u.raw_video;
    int width = video.display.line_width;
    int height = video.display.line_count;
    if (width <= 0 || height <= 0)
        return;
    auto frame = VideoFrameHaiku::create(VideoFrameHaiku::Format::I420, { width, height }, currentMediaTime());
    auto& y = frame->plane(0);
    auto& u = frame->plane(1);
    auto& v = frame->plane(2);
    int status = -1;
    switch (video.display.format) {
    case B_RGB32:
    case B_RGBA32: {
        size_t rowBytes = video.display.bytes_per_row ? video.display.bytes_per_row : 4 * width;
        if (bytes < rowBytes * height)
            return;
        status = libyuv::ARGBToI420(static_cast<const uint8_t*>(data), rowBytes, y.data, y.stride, u.data, u.stride, v.data, v.stride, width, height);
        break;
    }
    case B_YCbCr422: {
        size_t rowBytes = video.display.bytes_per_row ? video.display.bytes_per_row : 2 * width;
        if (bytes < rowBytes * height)
            return;
        status = libyuv::YUY2ToI420(static_cast<const uint8_t*>(data), rowBytes, y.data, y.stride, u.data, u.stride, v.data, v.stride, width, height);
        break;
    }
    default:
        break;
    }
    if (status)
        return;

    if (IntSize(width, height) != intrinsicSize()) {
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }, width, height] {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->setIntrinsicSize({ width, height });
        });
    }
    dispatchVideoFrameToObservers(frame.get(), { });
}

// Screen.

CaptureSourceOrError HaikuDisplayCaptureSource::create(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    auto id = device.persistentId();
    int32_t screenId = B_MAIN_SCREEN_ID.id;
    int32_t windowToken = -1;
    IntSize size;
    if (id.startsWith("haiku-screen:"_s)) {
        screenId = parseInteger<int32_t>(StringView(id).substring(13)).value_or(B_MAIN_SCREEN_ID.id);
        BScreen screen(screen_id { screenId });
        if (!screen.IsValid())
            return CaptureSourceOrError(CaptureSourceError { "The screen is gone"_s, MediaAccessDenialReason::HardwareError });
        BRect frame = screen.Frame();
        size = IntSize(frame.IntegerWidth() + 1, frame.IntegerHeight() + 1);
    } else if (id.startsWith("haiku-window:"_s)) {
        windowToken = parseInteger<int32_t>(StringView(id).substring(13)).value_or(-1);
        client_window_info* info = windowToken >= 0 ? get_window_info(windowToken) : nullptr;
        if (!info)
            return CaptureSourceOrError(CaptureSourceError { "The window is gone"_s, MediaAccessDenialReason::HardwareError });
        size = IntSize(info->window_right - info->window_left + 1, info->window_bottom - info->window_top + 1);
        free(info);
    } else
        return CaptureSourceOrError(CaptureSourceError { "Unknown screen"_s, MediaAccessDenialReason::PermissionDenied });

    auto source = adoptRef(*new HaikuDisplayCaptureSource(device, screenId, windowToken, size, WTF::move(hashSalts), pageIdentifier));
    if (constraints) {
        if (auto error = source->applyConstraints(*constraints))
            return CaptureSourceOrError(CaptureSourceError { error->invalidConstraint });
    }
    return CaptureSourceOrError(WTF::move(source));
}

HaikuDisplayCaptureSource::HaikuDisplayCaptureSource(const CaptureDevice& device, int32_t screenId, int32_t windowToken, IntSize size, MediaDeviceHashSalts&& hashSalts, std::optional<PageIdentifier> pageIdentifier)
    : RealtimeVideoCaptureSource(device, WTF::move(hashSalts), pageIdentifier)
    , m_screenId(screenId)
    , m_windowToken(windowToken)
    , m_screenSize(size)
{
    setIntrinsicSize(size, false);
    setFrameRate(15);
}

HaikuDisplayCaptureSource::~HaikuDisplayCaptureSource()
{
    stopProducingData();
}

void HaikuDisplayCaptureSource::generatePresets()
{
    Vector<VideoPreset> presets;
    presets.append(VideoPreset { { m_screenSize, { { 1, 30 } } } });
    setSupportedPresets(WTF::move(presets));
}

const RealtimeMediaSourceCapabilities& HaikuDisplayCaptureSource::capabilities()
{
    if (!m_capabilities) {
        RealtimeMediaSourceCapabilities capabilities(settings().supportedConstraints());
        capabilities.setDeviceId(hashedId());
        capabilities.setWidth({ 1, m_screenSize.width() });
        capabilities.setHeight({ 1, m_screenSize.height() });
        capabilities.setFrameRate({ 1, 30 });
        m_capabilities = WTF::move(capabilities);
    }
    return *m_capabilities;
}

const RealtimeMediaSourceSettings& HaikuDisplayCaptureSource::settings()
{
    if (!m_currentSettings) {
        RealtimeMediaSourceSettings settings;
        settings.setDeviceId(hashedId());
        settings.setLabel(name());
        settings.setDisplaySurface(m_windowToken >= 0 ? DisplaySurfaceType::Window : DisplaySurfaceType::Monitor);
        settings.setLogicalSurface(false);
        RealtimeMediaSourceSupportedConstraints supportedConstraints;
        supportedConstraints.setSupportsDeviceId(true);
        supportedConstraints.setSupportsWidth(true);
        supportedConstraints.setSupportsHeight(true);
        supportedConstraints.setSupportsAspectRatio(true);
        supportedConstraints.setSupportsFrameRate(true);
        supportedConstraints.setSupportsDisplaySurface(true);
        supportedConstraints.setSupportsLogicalSurface(true);
        settings.setSupportedConstraints(supportedConstraints);
        m_currentSettings = WTF::move(settings);
    }
    m_currentSettings->setWidth(size().width());
    m_currentSettings->setHeight(size().height());
    m_currentSettings->setFrameRate(frameRate());
    return *m_currentSettings;
}

void HaikuDisplayCaptureSource::configurationChanged()
{
    m_currentSettings = std::nullopt;
    m_capabilities = std::nullopt;
    RealtimeMediaSource::configurationChanged();
}

void HaikuDisplayCaptureSource::startProducingData()
{
    {
        Locker locker { m_lock };
        if (m_running)
            return;
        m_running = true;
    }
    m_captureFrameRate = frameRate() > 0 ? frameRate() : 15;
    auto outputSize = size();
    m_outputWidth = outputSize.width();
    m_outputHeight = outputSize.height();
    m_thread = Thread::create("Summit screen capture"_s, [protectedThis = Ref { *this }] {
        protectedThis->captureLoop();
    });
}

void HaikuDisplayCaptureSource::stopProducingData()
{
    {
        Locker locker { m_lock };
        if (!m_running)
            return;
        m_running = false;
        m_condition.notifyAll();
    }
    if (m_thread) {
        if (&Thread::currentSingleton() != m_thread.get())
            m_thread->waitForCompletion();
        m_thread = nullptr;
    }
}

void HaikuDisplayCaptureSource::settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag> flags)
{
    m_currentSettings = std::nullopt;
    if (flags.containsAny({ RealtimeMediaSourceSettings::Flag::Width, RealtimeMediaSourceSettings::Flag::Height })) {
        auto outputSize = size();
        m_outputWidth = outputSize.width();
        m_outputHeight = outputSize.height();
    }
    if (flags.contains(RealtimeMediaSourceSettings::Flag::FrameRate) && frameRate() > 0)
        m_captureFrameRate = frameRate();
}

// The largest size of `size`'s shape that fits `box` (no larger than `size`),
// even, as encoders want.
static IntSize fitWithin(IntSize size, IntSize box)
{
    if (box.isEmpty() || (size.width() <= box.width() && size.height() <= box.height()))
        return size;
    double scale = std::min(double(box.width()) / size.width(), double(box.height()) / size.height());
    return { std::max(2, int(size.width() * scale) & ~1), std::max(2, int(size.height() * scale) & ~1) };
}

// Rows read at a time (even, see captureLoop()).
static constexpr int screenBandRows = 128;

void HaikuDisplayCaptureSource::captureLoop()
{
    BScreen screen(screen_id { m_screenId });
    if (!screen.IsValid()) {
        callOnMainThread([protectedThis = Ref { *this }] {
            protectedThis->captureFailed();
        });
        return;
    }

    // A whole band and the last one, kept between pictures.
    std::unique_ptr<BBitmap> bandBitmaps[2];
    IntSize lastSize = m_screenSize;
    auto next = MonotonicTime::now();
    while (true) {
        {
            Locker locker { m_lock };
            if (!m_running)
                break;
            if (MonotonicTime::now() < next)
                m_condition.waitUntil(m_lock, next);
            if (!m_running)
                break;
        }
        double rate = std::clamp<double>(m_captureFrameRate, 1, 30);
        next = std::max(next + Seconds(1 / rate), MonotonicTime::now());

        if (!isProducingData() || muted())
            continue;

        BRect area = screen.Frame();
        if (m_windowToken >= 0) {
            client_window_info* info = get_window_info(m_windowToken);
            if (!info) {
                // The window was closed: so is the track.
                callOnMainThread([protectedThis = Ref { *this }] {
                    protectedThis->captureFailed();
                });
                break;
            }
            bool visible = !info->is_mini && info->show_hide_level <= 0 && (info->workspaces & (1u << current_workspace()));
            area = BRect(info->window_left, info->window_top, info->window_right, info->window_bottom) & screen.Frame();
            free(info);
            // Hidden or on another workspace: the last picture stays.
            if (!visible || !area.IsValid())
                continue;
        }

        int width = area.IntegerWidth() + 1;
        int height = area.IntegerHeight() + 1;
        auto picture = VideoFrameHaiku::create(VideoFrameHaiku::Format::I420, { width, height }, currentMediaTime());
        auto& y = picture->plane(0);
        auto& u = picture->plane(1);
        auto& v = picture->plane(2);

        // Reading the screen holds app_server's frame buffer, and every
        // window waits meanwhile: a 3840x1080 desktop drawn at twice that
        // takes a quarter of a second on the X399, whose frame buffer is in
        // video memory (uncached reads). So the picture is read in bands,
        // with a pause after each at least twice as long as the read: the
        // desktop keeps two thirds of app_server's time whatever the rate
        // asked for, and no wait is longer than one band.
        bool complete = true;
        for (int top = 0; top < height && complete; top += screenBandRows) {
            int rows = std::min(screenBandRows, height - top);
            auto& bitmap = bandBitmaps[rows == screenBandRows ? 0 : 1];
            if (!bitmap || bitmap->Bounds().IntegerWidth() + 1 != width || bitmap->Bounds().IntegerHeight() + 1 != rows) {
                bitmap = std::make_unique<BBitmap>(BRect(0, 0, width - 1, rows - 1), B_RGB32);
                if (bitmap->InitCheck() != B_OK) {
                    bitmap = nullptr;
                    complete = false;
                    break;
                }
            }
            BRect band(area.left, area.top + top, area.right, area.top + top + rows - 1);
            auto readStart = MonotonicTime::now();
            if (screen.ReadBitmap(bitmap.get(), true, &band) != B_OK) {
                complete = false;
                break;
            }
            auto readTime = MonotonicTime::now() - readStart;
            // Rows are even except at the bottom, so the chroma rows line up.
            if (libyuv::ARGBToI420(static_cast<const uint8_t*>(bitmap->Bits()), bitmap->BytesPerRow(),
                y.data + top * y.stride, y.stride, u.data + (top / 2) * u.stride, u.stride, v.data + (top / 2) * v.stride, v.stride,
                width, rows)) {
                complete = false;
                break;
            }
            Locker locker { m_lock };
            if (!m_running) {
                complete = false;
                break;
            }
            m_condition.waitFor(m_lock, std::max(readTime * 2, 1_ms));
            if (!m_running)
                complete = false;
        }
        if (!complete)
            continue;
        // The next picture is due one frame after this one started, or now.
        next = std::max(next, MonotonicTime::now());

        IntSize captured(width, height);
        if (captured != lastSize) {
            lastSize = captured;
            callOnMainThread([protectedThis = Ref { *this }, captured] {
                protectedThis->setIntrinsicSize(captured);
            });
        }

        RefPtr<VideoFrameHaiku> output = picture.ptr();
        IntSize outputSize = fitWithin(captured, IntSize(m_outputWidth, m_outputHeight));
        if (outputSize != captured)
            output = picture->resizedTo(outputSize);
        if (output)
            dispatchVideoFrameToObservers(*output, { });
    }
}

// Microphone.

CaptureSourceOrError HaikuAudioCaptureSource::create(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
    auto source = adoptRef(*new HaikuAudioCaptureSource(device, WTF::move(hashSalts), pageIdentifier));
    if (constraints) {
        if (auto error = source->applyConstraints(*constraints))
            return CaptureSourceOrError(CaptureSourceError { error->invalidConstraint });
    }
    return CaptureSourceOrError(WTF::move(source));
}

HaikuAudioCaptureSource::HaikuAudioCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& hashSalts, std::optional<PageIdentifier> pageIdentifier)
    : RealtimeMediaSource(device, WTF::move(hashSalts), pageIdentifier)
{
    initializeSampleRate(48000);
    initializeVolume(1);
    // Echo cancellation, noise suppression and gain control (libwebrtc's
    // audio processing) are on unless the page turns them off, as elsewhere.
    initializeEchoCancellation(HaikuVoiceProcessing::isAvailable());
    m_voiceProcessing = echoCancellation();
}

HaikuAudioCaptureSource::~HaikuAudioCaptureSource()
{
    m_connection = nullptr;
}

const RealtimeMediaSourceCapabilities& HaikuAudioCaptureSource::capabilities()
{
    if (!m_capabilities) {
        RealtimeMediaSourceCapabilities capabilities(settings().supportedConstraints());
        capabilities.setDeviceId(hashedId());
        capabilities.setGroupId(hashedGroupId());
        capabilities.setVolume({ 0.0, 1.0 });
        capabilities.setSampleRate({ 8000, 192000 });
        capabilities.setEchoCancellation(HaikuVoiceProcessing::isAvailable() ? RealtimeMediaSourceCapabilities::EchoCancellation::OnOrOff : RealtimeMediaSourceCapabilities::EchoCancellation::Off);
        m_capabilities = WTF::move(capabilities);
    }
    return *m_capabilities;
}

const RealtimeMediaSourceSettings& HaikuAudioCaptureSource::settings()
{
    if (!m_currentSettings) {
        RealtimeMediaSourceSettings settings;
        settings.setDeviceId(hashedId());
        settings.setGroupId(hashedGroupId());
        settings.setLabel(name());
        RealtimeMediaSourceSupportedConstraints supportedConstraints;
        supportedConstraints.setSupportsDeviceId(true);
        supportedConstraints.setSupportsGroupId(true);
        supportedConstraints.setSupportsEchoCancellation(true);
        supportedConstraints.setSupportsVolume(true);
        supportedConstraints.setSupportsSampleRate(true);
        settings.setSupportedConstraints(supportedConstraints);
        m_currentSettings = WTF::move(settings);
    }
    m_currentSettings->setVolume(volume());
    m_currentSettings->setSampleRate(sampleRate());
    m_currentSettings->setEchoCancellation(echoCancellation());
    return *m_currentSettings;
}

void HaikuAudioCaptureSource::configurationChanged()
{
    m_currentSettings = std::nullopt;
    m_capabilities = std::nullopt;
    RealtimeMediaSource::configurationChanged();
}

void HaikuAudioCaptureSource::settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag> flags)
{
    m_currentSettings = std::nullopt;
    if (flags.contains(RealtimeMediaSourceSettings::Flag::Volume))
        m_gain = volume();
    if (flags.contains(RealtimeMediaSourceSettings::Flag::EchoCancellation))
        m_voiceProcessing = echoCancellation() && HaikuVoiceProcessing::isAvailable();
}

void HaikuAudioCaptureSource::startProducingData()
{
    auto input = haikuMediaInputWithId(B_MEDIA_RAW_AUDIO, persistentID());
    if (!input) {
        captureFailed();
        return;
    }
    media_format format;
    format.type = B_MEDIA_RAW_AUDIO;
    format.u.raw_audio = media_raw_audio_format::wildcard;

    m_framesSent = 0;
    m_baseTime = MediaTime::invalidTime();
    m_gain = volume();
    String error;
    m_connection = HaikuCaptureConnection::connect(input->node, format, [weakThis = ThreadSafeWeakPtr { *this }](const void* data, size_t bytes, const media_format& format, bigtime_t) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->samplesReceived(data, bytes, format);
    }, error);
    if (!m_connection) {
        fprintf(stderr, "Summit: microphone \"%s\": %s\n", name().utf8().data(), error.utf8().data());
        captureFailed();
        return;
    }
    auto rate = m_connection->format().u.raw_audio.frame_rate;
    if (rate > 0 && rate != sampleRate()) {
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }, rate] {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->setSampleRate(rate);
        });
    }
}

void HaikuAudioCaptureSource::stopProducingData()
{
    m_connection = nullptr;
}

void HaikuAudioCaptureSource::samplesReceived(const void* data, size_t bytes, const media_format& format)
{
    if (!isProducingData())
        return;
    auto& audio = format.u.raw_audio;
    unsigned channels = audio.channel_count;
    double rate = audio.frame_rate;
    if (!channels || rate <= 0)
        return;

    size_t sampleSize = audio.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
    if (!sampleSize)
        return;
    size_t sampleCount = bytes / sampleSize;
    size_t frames = sampleCount / channels;
    if (!frames)
        return;
    m_samples.resize(frames * channels);
    float gain = m_gain;
    sampleCount = frames * channels;

    switch (audio.format) {
    case media_raw_audio_format::B_AUDIO_FLOAT: {
        auto* in = static_cast<const float*>(data);
        for (size_t i = 0; i < sampleCount; ++i)
            m_samples[i] = in[i] * gain;
        break;
    }
    case media_raw_audio_format::B_AUDIO_INT: {
        auto* in = static_cast<const int32_t*>(data);
        for (size_t i = 0; i < sampleCount; ++i)
            m_samples[i] = in[i] * (gain / 2147483648.0f);
        break;
    }
    case media_raw_audio_format::B_AUDIO_SHORT: {
        auto* in = static_cast<const int16_t*>(data);
        for (size_t i = 0; i < sampleCount; ++i)
            m_samples[i] = in[i] * (gain / 32768.0f);
        break;
    }
    case media_raw_audio_format::B_AUDIO_CHAR: {
        auto* in = static_cast<const int8_t*>(data);
        for (size_t i = 0; i < sampleCount; ++i)
            m_samples[i] = in[i] * (gain / 128.0f);
        break;
    }
    case media_raw_audio_format::B_AUDIO_UCHAR: {
        auto* in = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < sampleCount; ++i)
            m_samples[i] = (int(in[i]) - 128) * (gain / 128.0f);
        break;
    }
    default:
        return;
    }

    if (!m_voiceProcessing || rate != std::floor(rate) || std::fmod(rate, 100)) {
        m_pending.clear();
        sendSamples(m_samples.mutableSpan(), channels, rate);
        return;
    }

    // Voice processing takes 10 ms at a time.
    m_pending.append(m_samples.span());
    size_t chunk = static_cast<size_t>(rate / 100) * channels;
    size_t offset = 0;
    while (m_pending.size() - offset >= chunk) {
        auto samples = m_pending.mutableSpan().subspan(offset, chunk);
        HaikuVoiceProcessing::singleton().processCapture(samples, channels, static_cast<unsigned>(rate), true, true, true);
        sendSamples(samples, channels, rate);
        offset += chunk;
    }
    if (offset)
        m_pending.removeAt(0, offset);
}

void HaikuAudioCaptureSource::sendSamples(std::span<float> samples, unsigned channels, double rate)
{
    size_t frames = samples.size() / channels;
    if (m_baseTime.isInvalid())
        m_baseTime = currentMediaTime();
    auto time = m_baseTime + MediaTime(m_framesSent * 1000000 / uint64_t(rate), 1000000);
    m_framesSent += frames;

    HaikuAudioData audioData(samples, channels);
    HaikuAudioStreamDescription description(rate, channels);
    audioSamplesAvailable(time, audioData, description, frames);
}

// The center's platform factories.

AudioCaptureFactory& RealtimeMediaSourceCenter::defaultAudioCaptureFactory()
{
    return HaikuAudioCaptureFactory::singleton();
}

VideoCaptureFactory& RealtimeMediaSourceCenter::defaultVideoCaptureFactory()
{
    return HaikuVideoCaptureFactory::singleton();
}

DisplayCaptureFactory& RealtimeMediaSourceCenter::defaultDisplayCaptureFactory()
{
    return HaikuDisplayCaptureFactory::singleton();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && PLATFORM(HAIKU)
