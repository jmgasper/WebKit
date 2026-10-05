/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <Application.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace WebKit {

// This class owns only native data. Menu tracking and destruction take place
// on its worker; cancel never waits for that worker or touches a raw BMenu.
class NativeSelectPopupHaiku final {
public:
    struct Item {
        std::string label;
        bool enabled { true };
        bool groupLabel { false };
        bool separator { false };
    };

    NativeSelectPopupHaiku(std::vector<Item>&& items, BRect anchor, bool rightToLeft, int32 selectedIndex, std::function<void(int32)>&& completion)
        : m_control(std::make_shared<Control>(std::move(items), anchor, rightToLeft, selectedIndex, std::move(completion))) { }

    ~NativeSelectPopupHaiku() { cancel(); }

    static size_t activeCount() { return s_activeCount.load(); }

    status_t start()
    {
        if (m_started)
            return B_NOT_ALLOWED;
        m_started = true;
        auto* owner = new std::shared_ptr<Control>(m_control);
        ++s_activeCount;
        thread_id thread = spawn_thread([](void* data) -> int32 {
            std::unique_ptr<std::shared_ptr<Control>> owner(static_cast<std::shared_ptr<Control>*>(data));
            auto control = *owner;
            int32 index = -1;
            if (!control->cancelled && !control->items.empty()) {
                auto menu = std::make_unique<Menu>(control);
                if (menu->populate()) {
                    BPoint location = menu->location();
                    // BPopUpMenu's async Go has no completion or failure result.
                    // Its synchronous result is safe here, off the WebKit loop,
                    // and covers selection, Escape, outside click and failure.
                    BMenuItem* chosen = menu->Go(location, false, true, control->anchor, false);
                    if (chosen && !control->cancelled)
                        index = menu->IndexOf(chosen);
                }
                // Go has joined its own tracking thread. Destroy the native
                // menu before posting the completion to the application.
                menu.reset();
            }
            if (control->cancelled)
                index = -1;
            control->completion(index);
            --s_activeCount;
            return B_OK;
        }, "WebKit select popup", B_NORMAL_PRIORITY, owner);
        if (thread < B_OK) {
            --s_activeCount;
            delete owner;
            return thread;
        }
        status_t status = resume_thread(thread);
        if (status != B_OK) {
            kill_thread(thread);
            --s_activeCount;
            delete owner;
        }
        return status;
    }

    void cancel()
    {
        m_control->cancelled = true;
        sendEscape(target());
    }

    BMessenger target() const
    {
        std::lock_guard lock(m_control->mutex);
        return m_control->target;
    }

private:
    struct Control {
        Control(std::vector<Item>&& items, BRect anchor, bool rightToLeft, int32 selectedIndex, std::function<void(int32)>&& completion)
            : items(std::move(items)), anchor(anchor), rightToLeft(rightToLeft), selectedIndex(selectedIndex), completion(std::move(completion)) { }
        const std::vector<Item> items;
        const BRect anchor;
        const bool rightToLeft;
        const int32 selectedIndex;
        const std::function<void(int32)> completion;
        std::atomic<bool> cancelled { false };
        std::mutex mutex;
        BMessenger target;
    };

    static void sendEscape(BMessenger target)
    {
        if (!target.IsValid())
            return;
        BMessage message(B_KEY_DOWN);
        const char escape[] = { B_ESCAPE, 0 };
        message.AddString("bytes", escape);
        target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }

    class Menu final : public BPopUpMenu {
        class Option final : public BMenuItem {
        public:
            explicit Option(const char* label) : BMenuItem(label, nullptr) { }
            bool isHighlighted() const { return IsSelected(); }
        };

    public:
        explicit Menu(std::shared_ptr<Control> control)
            : BPopUpMenu("WebKit select", true, false), m_control(std::move(control))
        {
            SetTriggersEnabled(false);
            SetTrackingHook([](BMenu*, void* state) {
                return static_cast<Control*>(state)->cancelled.load();
            }, m_control.get());
        }

        bool populate()
        {
            for (size_t i = 0; i < m_control->items.size(); ++i) {
                const auto& source = m_control->items[i];
                if (source.separator) {
                    if (!AddSeparatorItem())
                        return false;
                    continue;
                }
                auto item = std::make_unique<Option>(source.label.c_str());
                item->SetEnabled(source.enabled && !source.groupLabel);
                item->SetMarked(static_cast<int32>(i) == m_control->selectedIndex);
                if (!AddItem(item.get()))
                    return false;
                item.release();
            }
            return true;
        }

        BPoint location()
        {
            BRect screen = BScreen().Frame();
            SetMaxContentWidth(std::max(1.0f, screen.Width() - 40));
            SetExplicitMinSize(BSize(std::clamp(m_control->anchor.Width(), 1.0f, std::max(1.0f, screen.Width() - 2)), B_SIZE_UNSET));
            DoLayout();
            BPoint point = m_control->anchor.LeftTop();
            if (m_control->rightToLeft)
                point.x = m_control->anchor.right - Bounds().Width();
            if (auto* marked = ItemAt(m_control->selectedIndex))
                point.y -= marked->Frame().top;
            point.x = std::clamp(point.x, screen.left, std::max(screen.left, screen.right - Bounds().Width()));
            // Oversized menus keep their full content height. BMenu provides
            // native screen clipping, scroll arrows and marked-item scrolling.
            point.y = std::clamp(point.y, screen.top, std::max(screen.top, screen.bottom - Bounds().Height()));
            return point;
        }

        void AttachedToWindow() override
        {
            BPopUpMenu::AttachedToWindow();
            BMessenger messenger(this);
            {
                std::lock_guard lock(m_control->mutex);
                m_control->target = messenger;
            }
            if (m_control->cancelled)
                sendEscape(messenger);
        }

        void KeyDown(const char* bytes, int32 count) override
        {
            if (count <= 0)
                return;
            bool highlighted = false;
            for (int32 i = 0; i < CountItems(); ++i) {
                if (auto* option = dynamic_cast<Option*>(ItemAt(i)); option && option->isHighlighted()) {
                    highlighted = true;
                    break;
                }
            }
            // BMenu deselects highlighted items when the pointer is outside
            // the menu. Begin keyboard navigation from the current value,
            // using native navigation to skip groups, disabled rows and rules.
            auto* selected = ItemAt(m_control->selectedIndex);
            if (!highlighted && selected && selected->IsEnabled()
                && (bytes[0] == B_UP_ARROW || bytes[0] == B_DOWN_ARROW || bytes[0] == B_ENTER || bytes[0] == B_SPACE)) {
                const char down = B_DOWN_ARROW;
                for (int32 i = 0; i <= m_control->selectedIndex; ++i) {
                    if (ItemAt(i)->IsEnabled())
                        BPopUpMenu::KeyDown(&down, 1);
                }
            }
            BPopUpMenu::KeyDown(bytes, count);
        }

        void DetachedFromWindow() override
        {
            {
                std::lock_guard lock(m_control->mutex);
                m_control->target = { };
            }
            BPopUpMenu::DetachedFromWindow();
        }

    private:
        const std::shared_ptr<Control> m_control;
    };

    const std::shared_ptr<Control> m_control;
    bool m_started { false };
    inline static std::atomic<size_t> s_activeCount { 0 };
};

} // namespace WebKit
