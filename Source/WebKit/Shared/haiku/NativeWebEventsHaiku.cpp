/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "NativeWebKeyboardEvent.h"
#include "NativeWebMouseEvent.h"
#include "NativeWebWheelEvent.h"

#include <InterfaceDefs.h>
#include <String.h>
#include <View.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/Scrollbar.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeWebKeyboardEvent);
WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeWebMouseEvent);
WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeWebWheelEvent);

static OptionSet<WebEventModifier> eventModifiers(const BMessage& message)
{
    auto native = message.FindInt32("modifiers");
    OptionSet<WebEventModifier> result;
    if (native & B_SHIFT_KEY)
        result.add(WebEventModifier::ShiftKey);
    // Match WebCore's Haiku mapping: the native Command key provides the web
    // Control shortcuts, while native Control provides the web Alt modifier.
    if (native & B_COMMAND_KEY)
        result.add(WebEventModifier::ControlKey);
    if (native & B_CONTROL_KEY)
        result.add(WebEventModifier::AltKey);
    if (native & B_OPTION_KEY)
        result.add(WebEventModifier::MetaKey);
    if (native & B_CAPS_LOCK)
        result.add(WebEventModifier::CapsLockKey);
    return result;
}

static WebEventData makeEventDataHaiku(const BMessage& message, WebEventType type)
{
    int64 when = 0;
    message.FindInt64("when", &when);
    return { type, eventModifiers(message), when > 0 ? MonotonicTime::fromRawSeconds(when / 1000000.0) : MonotonicTime::now() };
}

static bool validPosition(BPoint position)
{
    return std::isfinite(position.x) && std::isfinite(position.y)
        && std::abs(position.x) < 1e7f && std::abs(position.y) < 1e7f;
}

RefPtr<NativeWebKeyboardEvent> NativeWebKeyboardEvent::create(const BMessage& message)
{
    bool keyDown = message.what == B_KEY_DOWN || message.what == B_UNMAPPED_KEY_DOWN;
    if (!keyDown && message.what != B_KEY_UP && message.what != B_UNMAPPED_KEY_UP)
        return nullptr;
    BString bytes = message.FindString("bytes");
    auto keyCode = message.FindInt32("key");
    WebKeyboardEventData keyboard;
    keyboard.text = String::fromUTF8(std::span<const char> { bytes.String(), static_cast<size_t>(bytes.Length()) });
    if (keyboard.text.isNull() && bytes.Length())
        return nullptr;
    keyboard.unmodifiedText = keyboard.text;
    keyboard.key = PlatformKeyboardEvent::KeyValueForKeyEvent(bytes, keyCode);
    keyboard.code = PlatformKeyboardEvent::KeyCodeForKeyEvent(keyCode);
    keyboard.keyIdentifier = PlatformKeyboardEvent::keyIdentifierForHaikuKeyCode(bytes.ByteAt(0), keyCode);
    keyboard.windowsVirtualKeyCode = PlatformKeyboardEvent::windowsKeyCodeForKeyEvent(bytes.ByteAt(0), keyCode);
    keyboard.nativeVirtualKeyCode = keyCode;
    keyboard.isAutoRepeat = keyDown && message.FindInt32("be:key_repeat") > 0;
    keyboard.isKeypad = keyboard.code.startsWith("Numpad"_s);
    if (keyboard.text.isEmpty()) {
        if (keyboard.code.startsWith("Shift"_s))
            keyboard.key = "Shift"_s;
        else if (keyboard.code.startsWith("Control"_s))
            keyboard.key = "Alt"_s;
        else if (keyboard.code.startsWith("Alt"_s))
            keyboard.key = "Control"_s;
        else if (keyboard.code.startsWith("Meta"_s))
            keyboard.key = "Meta"_s;
        else if (keyboard.code == "CapsLock"_s || keyboard.code == "NumLock"_s)
            keyboard.key = keyboard.code;
        else
            keyboard.key = "Unidentified"_s;
    }
    return adoptRef(*new NativeWebKeyboardEvent({ makeEventDataHaiku(message, keyDown ? WebEventType::KeyDown : WebEventType::KeyUp), WTF::move(keyboard) }, message));
}

NativeWebKeyboardEvent::NativeWebKeyboardEvent(WebKeyboardEventInit&& init, const BMessage& message)
    : WebKeyboardEvent(WTF::move(init.event), WTF::move(init.keyboard))
    , m_nativeEvent(message)
{
}

static WebMouseEventButton mouseButtonForMask(uint32_t buttons)
{
    if (buttons & B_PRIMARY_MOUSE_BUTTON)
        return WebMouseEventButton::Left;
    if (buttons & B_SECONDARY_MOUSE_BUTTON)
        return WebMouseEventButton::Right;
    if (buttons & B_TERTIARY_MOUSE_BUTTON)
        return WebMouseEventButton::Middle;
    if (buttons & 8)
        return WebMouseEventButton::Back;
    if (buttons & 16)
        return WebMouseEventButton::Forward;
    return WebMouseEventButton::None;
}

RefPtr<NativeWebMouseEvent> NativeWebMouseEvent::create(const BMessage& message, BPoint viewPosition, BPoint screenPosition, uint32_t previousButtons)
{
    if ((message.what != B_MOUSE_DOWN && message.what != B_MOUSE_UP && message.what != B_MOUSE_MOVED)
        || !validPosition(viewPosition) || !validPosition(screenPosition))
        return nullptr;
    WebEventType type = message.what == B_MOUSE_DOWN ? WebEventType::MouseDown
        : message.what == B_MOUSE_UP ? WebEventType::MouseUp : WebEventType::MouseMove;
    auto buttons = static_cast<uint32_t>(message.FindInt32("buttons")) & 31;
    uint32_t changedButtons = buttons;
    if (type == WebEventType::MouseDown)
        changedButtons = buttons & ~previousButtons;
    else if (type == WebEventType::MouseUp)
        changedButtons = previousButtons & ~buttons;
    WebMouseEventData mouse;
    mouse.button = mouseButtonForMask(changedButtons);
    mouse.buttons = buttons;
    mouse.position = { viewPosition.x, viewPosition.y };
    mouse.globalPosition = { screenPosition.x, screenPosition.y };
    mouse.clickCount = type == WebEventType::MouseMove ? 0 : std::max(0, message.FindInt32("clicks"));
    return adoptRef(*new NativeWebMouseEvent({ makeEventDataHaiku(message, type), WTF::move(mouse) }, message));
}

NativeWebMouseEvent::NativeWebMouseEvent(WebMouseEventInit&& init, const BMessage& message)
    : WebMouseEvent(WTF::move(init.event), WTF::move(init.mouse))
    , m_nativeEvent(message)
{
}

// One notch of a Haiku mouse wheel is one tick. WebKit's line step (40 px)
// made each notch crawl, and a notch animated over 40 px easily reads as the
// wheel not working. Haiku's own scroll views move three small steps a notch;
// Chrome moves 100 px. SUMMIT_WHEEL_PIXELS_PER_NOTCH (10 to 400) overrides it.
static float wheelPixelsPerNotch()
{
    static float pixels = [] {
        if (const char* value = std::getenv("SUMMIT_WHEEL_PIXELS_PER_NOTCH")) {
            float requested = std::strtof(value, nullptr);
            if (requested >= 10 && requested <= 400)
                return requested;
        }
        return 2.5f * Scrollbar::pixelsPerLineStep();
    }();
    return pixels;
}

RefPtr<NativeWebWheelEvent> NativeWebWheelEvent::create(const BMessage& message, BPoint viewPosition, BPoint screenPosition)
{
    if (message.what != B_MOUSE_WHEEL_CHANGED || !validPosition(viewPosition) || !validPosition(screenPosition))
        return nullptr;
    float deltaX = message.FindFloat("be:wheel_delta_x");
    float deltaY = message.FindFloat("be:wheel_delta_y");
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY) || std::abs(deltaX) > 1e6f || std::abs(deltaY) > 1e6f)
        return nullptr;
    WebWheelEventData wheel;
    wheel.position = { static_cast<int>(std::round(viewPosition.x)), static_cast<int>(std::round(viewPosition.y)) };
    wheel.globalPosition = { static_cast<int>(std::round(screenPosition.x)), static_cast<int>(std::round(screenPosition.y)) };
    wheel.wheelTicks = { -deltaX, -deltaY };
    wheel.delta = wheel.wheelTicks;
    wheel.delta.scale(wheelPixelsPerNotch());
    wheel.granularity = WebWheelEventGranularity::ScrollByPixelWheelEvent;
    return adoptRef(*new NativeWebWheelEvent({ makeEventDataHaiku(message, WebEventType::Wheel), WTF::move(wheel) }, message));
}

NativeWebWheelEvent::NativeWebWheelEvent(WebWheelEventInit&& init, const BMessage& message)
    : WebWheelEvent(WTF::move(init.event), WTF::move(init.wheel))
    , m_nativeEvent(message)
{
}

} // namespace WebKit
