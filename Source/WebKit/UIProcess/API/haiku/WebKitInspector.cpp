/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// BWebKitInspectorSession (WebKitInspector.h): a Web Inspector protocol
// frontend for a host's own developer tools.

#include "config.h"
#include "WebKitInspector.h"

#include "WebKitView.h"
#include "WebPageInspectorController.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebViewPrivate.h"
#include "WebViewStateHaiku.h"
#include "WebsiteDataStore.h"
#include <JavaScriptCore/InspectorFrontendChannel.h>
#include <Message.h>
#include <mutex>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

using namespace WebKit;

namespace {

// What may wait for Take(). A host that stops taking loses messages rather
// than the browser its memory.
constexpr size_t maximumWaitingBytesHaiku = 256 * 1024 * 1024;

// The page's messages, from the WebKit main thread to the host's thread.
struct InspectorQueueHaiku {
    InspectorQueueHaiku(BMessenger target, uint64 identifier)
        : target(target)
        , identifier(identifier)
    {
    }

    void append(const String& message)
    {
        auto utf8 = message.utf8();
        bool announce = false;
        {
            std::lock_guard guard(mutex);
            if (bytes + utf8.length() > maximumWaitingBytesHaiku) {
                overflowed = true;
                return;
            }
            bytes += utf8.length();
            messages.emplace_back(utf8.legacyCStringPointer() ? utf8.legacyCStringPointer() : "", utf8.length());
            announce = !std::exchange(announced, true);
        }
        if (!announce)
            return;
        BMessage notification(B_WEBKIT_INSPECTOR_MESSAGES);
        notification.AddUInt64("identifier", identifier);
        // A full port must not stop the WebKit main thread. The next message
        // announces again.
        if (target.SendMessage(&notification, static_cast<BHandler*>(nullptr), 0) != B_OK) {
            std::lock_guard guard(mutex);
            announced = false;
        }
    }

    std::mutex mutex;
    std::vector<std::string> messages;
    size_t bytes { 0 };
    bool announced { false };
    bool overflowed { false };
    const BMessenger target;
    const uint64 identifier;
};

class InspectorChannelHaiku final : public Inspector::FrontendChannel {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(InspectorChannelHaiku);
public:
    explicit InspectorChannelHaiku(std::shared_ptr<InspectorQueueHaiku> queue)
        : m_queue(std::move(queue))
    {
    }

private:
    // The host's tools are not WebKit's own inspector page, which is what
    // a local connection means to the engine.
    ConnectionType connectionType() const final { return ConnectionType::Remote; }
    void sendMessageToFrontend(const String& message) final { m_queue->append(message); }

    const std::shared_ptr<InspectorQueueHaiku> m_queue;
};

// Sessions connected to pages of the same preferences; the WebKit main
// thread's. Developer extras are for as long as somebody is looking.
HashMap<WebPreferences*, unsigned>& sessionsOfPreferencesHaiku()
{
    static NeverDestroyed<HashMap<WebPreferences*, unsigned>> sessions;
    return sessions;
}

} // namespace

struct BWebKitInspectorSession::State {
    std::shared_ptr<WebViewStateHaiku> view;
    std::shared_ptr<InspectorQueueHaiku> queue;
    // The rest is read and written on the WebKit main thread only.
    std::unique_ptr<InspectorChannelHaiku> channel;
    WeakPtr<WebPageProxy> page;
    RefPtr<WebPreferences> preferences;
};

std::shared_ptr<BWebKitInspectorSession> BWebKitInspectorSession::Create(BWebKitView& view, BMessenger target, uint64 identifier)
{
    auto state = std::make_shared<State>();
    state->view = view.m_state;
    state->queue = std::make_shared<InspectorQueueHaiku>(target, identifier);
    std::shared_ptr<BWebKitInspectorSession> session(new BWebKitInspectorSession(state));
    if (view.InitCheck() != B_OK)
        return session;
    WTF::RunLoop::mainSingleton().dispatch([state] {
        if (state->view->closed || !state->view->client)
            return;
        RefPtr page = state->view->client->page();
        if (!page || page->isClosed())
            return;
        // Without them the page's console messages stay in the web process.
        state->preferences = page->preferences();
        if (sessionsOfPreferencesHaiku().add(state->preferences.get(), 0).iterator->value++ == 0)
            state->preferences->setDeveloperExtrasEnabled(true);
        state->channel = makeUnique<InspectorChannelHaiku>(state->queue);
        state->page = *page;
        page->inspectorController().connectFrontend(*state->channel);
    });
    return session;
}

BWebKitInspectorSession::BWebKitInspectorSession(std::shared_ptr<State> state)
    : m_state(std::move(state))
{
}

BWebKitInspectorSession::~BWebKitInspectorSession()
{
    WTF::RunLoop::mainSingleton().dispatch([state = std::move(m_state)] {
        if (!state->channel)
            return;
        // A closed page has disconnected its frontends itself.
        if (RefPtr page = state->page.get(); page && !page->isClosed())
            page->inspectorController().disconnectFrontend(*state->channel);
        state->channel = nullptr;
        auto& sessions = sessionsOfPreferencesHaiku();
        if (auto found = sessions.find(state->preferences.get()); found != sessions.end() && !--found->value) {
            sessions.remove(found);
            state->preferences->setDeveloperExtrasEnabled(false);
        }
        state->preferences = nullptr;
    });
}

void BWebKitInspectorSession::Send(const char* message)
{
    if (!message || !*message)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, message = std::string(message)] {
        if (!state->channel)
            return;
        RefPtr page = state->page.get();
        if (!page || page->isClosed())
            return;
        page->inspectorController().dispatchMessageFromFrontend(String::fromUTF8WithLatin1Fallback(std::span<const char> { message.data(), message.size() }));
    });
}

bool BWebKitInspectorSession::Take(std::vector<std::string>& messages)
{
    auto& queue = *m_state->queue;
    std::lock_guard guard(queue.mutex);
    queue.announced = false;
    if (queue.messages.empty())
        return false;
    if (messages.empty())
        messages.swap(queue.messages);
    else {
        messages.insert(messages.end(), std::make_move_iterator(queue.messages.begin()), std::make_move_iterator(queue.messages.end()));
        queue.messages.clear();
    }
    queue.bytes = 0;
    return true;
}

bool BWebKitInspectorSession::Overflowed() const
{
    auto& queue = *m_state->queue;
    std::lock_guard guard(queue.mutex);
    return queue.overflowed;
}
