/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <Application.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace WebKit {

// Owns only native objects. Native workers never inspect WebKit references;
// their bridge callbacks must submit engine work to its owning run loop.
class NativeExtensionPopupHaiku final {
public:
    enum class Event { Created, Shown, Closed };
    using Notification = std::function<void(Event, status_t)>;
    using ViewFactory = std::function<BView*(BRect)>;

    NativeExtensionPopupHaiku(BMessenger owner, BRect anchor, std::string title, ViewFactory factory, Notification notification, std::function<bool()> hasModalChild = { })
        : m_control(std::make_shared<Control>(owner, anchor, std::move(title), std::move(factory), std::move(notification), std::move(hasModalChild))) { }
    ~NativeExtensionPopupHaiku() { cancel(); }
    static uint32_t activeCount() { return s_active.load(); }

    status_t start()
    {
        if (std::exchange(m_started, true))
            return B_NOT_ALLOWED;
        auto anchor = m_control->anchor;
        if (!m_control->factory || !m_control->notification || !anchor.IsValid()
            || !std::isfinite(anchor.left) || !std::isfinite(anchor.top) || !std::isfinite(anchor.right) || !std::isfinite(anchor.bottom))
            return B_BAD_VALUE;
        auto* data = new std::shared_ptr<Control>(m_control);
        ++s_active;
        auto thread = spawn_thread([](void* pointer) -> int32 {
            std::unique_ptr<std::shared_ptr<Control>> data(static_cast<std::shared_ptr<Control>*>(pointer));
            auto control = *data;
            status_t status = B_TIMED_OUT;
            const auto deadline = system_time() + 2000000;
            while (!control->cancelled && system_time() < deadline) {
                status = control->owner.LockTargetWithTimeout(50000);
                if (status == B_OK)
                    break;
                if (status != B_TIMED_OUT && status != B_WOULD_BLOCK)
                    break;
            }
            if (control->cancelled) {
                if (status == B_OK) {
                    BLooper* looper = nullptr;
                    control->owner.Target(&looper);
                    if (looper)
                        looper->Unlock();
                }
                finish(control, B_CANCELED);
            } else if (status != B_OK)
                finish(control, status);
            else {
                BLooper* looper = nullptr;
                auto* owner = dynamic_cast<BWindow*>(control->owner.Target(&looper));
                if (!owner || owner != looper || owner->IsHidden() || owner->IsMinimized()) {
                    if (looper)
                        looper->Unlock();
                    finish(control, B_BAD_VALUE);
                } else {
                    auto* window = new Window(control);
                    status = window->AddToSubset(owner);
                    looper->Unlock();
                    if (status == B_OK && !control->cancelled && window->hasContent()) {
                        {
                            std::lock_guard guard(control->mutex);
                            control->target = BMessenger(window);
                        }
                        control->notification(Event::Created, B_OK);
                        if (window->Run() < B_OK) {
                            control->closeStatus = B_ERROR;
                            delete window;
                        }
                        // Run transferred window lifetime to its native looper.
                    } else {
                        control->closeStatus = status != B_OK ? status : (control->cancelled ? B_CANCELED : B_NO_INIT);
                        delete window;
                    }
                }
            }
            --s_active;
            return B_OK;
        }, "WebKit extension popup", B_NORMAL_PRIORITY, data);
        if (thread < B_OK) {
            --s_active;
            delete data;
            return thread;
        }
        auto status = resume_thread(thread);
        if (status != B_OK) {
            kill_thread(thread);
            --s_active;
            delete data;
        }
        return status;
    }

    void show(bool requireFocusedOwner = false)
    {
        m_control->requireFocusedOwner = requireFocusedOwner;
        m_control->showRequested = true;
        wake();
    }
    void cancel() { m_control->cancelled = true; wake(); }
    void resize(BSize size)
    {
        if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0)
            return;
        {
            std::lock_guard guard(m_control->mutex);
            m_control->size = size;
            ++m_control->sizeGeneration;
        }
        wake();
    }
    BMessenger target() const
    {
        std::lock_guard guard(m_control->mutex);
        return m_control->target;
    }

    static BRect frameForSize(BRect anchor, BSize size, BRect screen)
    {
        auto maximumWidth = std::max(1.0f, std::min(800.0f, screen.Width() - 16));
        auto maximumHeight = std::max(1.0f, std::min(600.0f, screen.Height() - 16));
        auto width = std::clamp(std::isfinite(size.width) ? std::ceil(size.width) : 360.0f, std::min(25.0f, maximumWidth), maximumWidth);
        auto height = std::clamp(std::isfinite(size.height) ? std::ceil(size.height) : 240.0f, std::min(25.0f, maximumHeight), maximumHeight);
        auto x = anchor.right - width + 1;
        auto y = anchor.bottom + 1;
        if (y + height > screen.bottom && anchor.top - height >= screen.top)
            y = anchor.top - height;
        x = std::clamp(x, screen.left, std::max(screen.left, screen.right - width + 1));
        y = std::clamp(y, screen.top, std::max(screen.top, screen.bottom - height + 1));
        return { x, y, x + width - 1, y + height - 1 };
    }

private:
    static constexpr uint32 updateMessage = 'expu';
    struct Control {
        Control(BMessenger owner, BRect anchor, std::string title, ViewFactory factory, Notification notification, std::function<bool()> hasModalChild)
            : owner(owner), anchor(anchor), title(std::move(title)), factory(std::move(factory)), notification(std::move(notification)), hasModalChild(std::move(hasModalChild)) { }
        const BMessenger owner;
        const BRect anchor;
        const std::string title;
        const ViewFactory factory;
        const Notification notification;
        const std::function<bool()> hasModalChild;
        std::mutex mutex;
        BMessenger target;
        BSize size { 360, 240 };
        uint64_t sizeGeneration { 1 };
        std::atomic<bool> cancelled { false };
        std::atomic<bool> showRequested { false };
        std::atomic<bool> requireFocusedOwner { false };
        std::atomic<bool> finished { false };
        std::atomic<status_t> closeStatus { B_OK };
    };
    static void finish(const std::shared_ptr<Control>& control, status_t status)
    {
        if (!control->finished.exchange(true))
            control->notification(Event::Closed, status);
    }
    void wake()
    {
        BMessage message(updateMessage);
        target().SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
        // The periodic native update also observes state if the port is full.
    }
    class Activity {
    protected:
        Activity() { ++s_active; }
        ~Activity() { --s_active; }
    };
    class CompletionAfterDestruction {
    protected:
        explicit CompletionAfterDestruction(std::shared_ptr<Control> control) : m_control(std::move(control)) { }
        ~CompletionAfterDestruction() { finish(m_control, m_control->closeStatus); }
    private:
        std::shared_ptr<Control> m_control;
    };
    // Complete after the native window and all child views have been destroyed;
    // keep activity nonzero until the completion has been submitted.
    class Window final : private Activity, private CompletionAfterDestruction, public BWindow {
    public:
        explicit Window(std::shared_ptr<Control> control)
            : CompletionAfterDestruction(control)
            , BWindow(frameForSize(control->anchor, BSize(360, 240), BScreen().Frame()), control->title.c_str(), B_BORDERED_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL,
                B_ASYNCHRONOUS_CONTROLS | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_MOVABLE | B_CLOSE_ON_ESCAPE)
            , m_control(std::move(control))
        {
            if (!m_control->cancelled) {
                m_content = m_control->factory(Bounds());
                if (m_content) {
                    AddChild(m_content);
                    m_content->MakeFocus();
                }
            }
            BMessage update(updateMessage);
            m_update = std::make_unique<BMessageRunner>(BMessenger(this), &update, 100000);
        }
        bool hasContent() const { return m_content && m_content->Window() == this && m_update->InitCheck() == B_OK; }
        bool QuitRequested() override { m_control->cancelled = true; return true; }
        void WindowActivated(bool active) override
        {
            BWindow::WindowActivated(active);
            if (active) {
                m_activated = true;
                m_deactivatedAt = 0;
            } else if (m_activated)
                m_deactivatedAt = system_time();
        }
        void MessageReceived(BMessage* message) override
        {
            if (message->what != updateMessage) {
                BWindow::MessageReceived(message);
                return;
            }
            bool ownerAvailable = m_control->owner.IsValid();
            if (ownerAvailable && m_control->owner.LockTargetWithTimeout(0) == B_OK) {
                BLooper* looper = nullptr;
                auto* owner = dynamic_cast<BWindow*>(m_control->owner.Target(&looper));
                ownerAvailable = owner && !owner->IsHidden() && !owner->IsMinimized();
                if (looper)
                    looper->Unlock();
            }
            bool lostFocus = m_deactivatedAt && system_time() - m_deactivatedAt >= 100000
                && !(m_control->hasModalChild && m_control->hasModalChild());
            if (m_control->cancelled || !ownerAvailable || lostFocus) {
                PostMessage(B_QUIT_REQUESTED);
                return;
            }
            BSize size;
            uint64_t generation;
            {
                std::lock_guard guard(m_control->mutex);
                size = m_control->size;
                generation = m_control->sizeGeneration;
            }
            if (generation != m_sizeGeneration) {
                m_sizeGeneration = generation;
                auto rect = frameForSize(m_control->anchor, size, BScreen(this).Frame());
                ResizeTo(rect.Width(), rect.Height());
                MoveTo(rect.LeftTop());
            }
            if (!m_shown && m_control->showRequested) {
                // A programmatic request can outlive its owner's focus while
                // waiting for this native looper. Check at presentation time;
                // a busy owner is retried by the periodic update.
                if (m_control->requireFocusedOwner) {
                    if (m_control->owner.LockTargetWithTimeout(0) != B_OK)
                        return;
                    BLooper* looper = nullptr;
                    auto* owner = dynamic_cast<BWindow*>(m_control->owner.Target(&looper));
                    bool focused = owner && owner->IsActive() && !owner->IsHidden() && !owner->IsMinimized();
                    if (looper)
                        looper->Unlock();
                    if (!focused) {
                        m_control->cancelled = true;
                        m_control->closeStatus = B_CANCELED;
                        PostMessage(B_QUIT_REQUESTED);
                        return;
                    }
                }
                m_shown = true;
                Show();
                Activate(true);
                m_content->MakeFocus();
                m_control->notification(Event::Shown, B_OK);
            }
        }
    private:
        const std::shared_ptr<Control> m_control;
        std::unique_ptr<BMessageRunner> m_update;
        BView* m_content { nullptr };
        uint64_t m_sizeGeneration { 0 };
        bigtime_t m_deactivatedAt { 0 };
        bool m_shown { false };
        bool m_activated { false };
    };
    const std::shared_ptr<Control> m_control;
    bool m_started { false };
    static inline std::atomic<uint32_t> s_active { 0 };
};
} // namespace WebKit
