/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// BWebKitView's embedding interfaces (WebKitEmbedding.h): user scripts,
// script messages with replies, script evaluation and URL scheme handlers.

#include "config.h"
#include "WebKitView.h"

#include "APIArray.h"
#include "APIContentWorld.h"
#include "APIDictionary.h"
#include "APIFrameInfo.h"
#include "APIHTTPCookieStore.h"
#include "APINumber.h"
#include "APIPageConfiguration.h"
#include "APIString.h"
#include "APIUserScript.h"
#include "FrameInfoData.h"
#include "InjectUserScriptImmediately.h"
#include "JavaScriptEvaluationResult.h"
#include "RunJavaScriptParameters.h"
#include "WebPageProxy.h"
#include "WebProcessPool.h"
#include "WebScriptMessageHandler.h"
#include "WebURLSchemeHandler.h"
#include "WebURLSchemeTask.h"
#include "WebUserContentControllerProxy.h"
#include "EmbeddingHaiku.h"
#include "WebViewPrivate.h"
#include "WebViewStateHaiku.h"
#include "WebsiteDataStore.h"
#include "WebsiteDataType.h"
#include <Message.h>
#include <WebCore/Cookie.h>
#include <WebCore/ExceptionDetails.h>
#include <WebCore/FormData.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/RunJavaScriptParameters.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/UserScript.h>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <wtf/HashMap.h>
#include <wtf/JSONValues.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/URLParser.h>
#include <wtf/WallTime.h>
#include <wtf/text/StringBuilder.h>

using namespace WebKit;
using namespace WebCore;

namespace {

using ScriptReplyHaiku = CompletionHandler<void(std::expected<JavaScriptEvaluationResult, String>&&)>;

std::string copyTextHaiku(const char* text)
{
    return text ? std::string(text) : std::string();
}

String stringHaiku(const std::string& text)
{
    return String::fromUTF8WithLatin1Fallback(std::span<const char> { text.data(), text.size() });
}

// Main thread only: the page of a live view, or null.
WebPageProxy* pageHaiku(const std::shared_ptr<WebViewStateHaiku>& state)
{
    if (state->closed || !state->client)
        return nullptr;
    return state->client->page();
}

BMessenger viewOfStateHaiku(const std::shared_ptr<WebViewStateHaiku>& state)
{
    std::lock_guard guard(state->mutex);
    return state->view;
}

// The native view showing a page. Every page client of this port is a WebView;
// pages opened by window.open() share their opener's scripts and handlers, so
// a message names the view it came from rather than the one that registered.
BMessenger viewOfPageHaiku(WebPageProxy& page)
{
    if (auto* client = page.pageClient())
        return static_cast<WebView*>(client)->nativeView();
    return BMessenger();
}

void addStringHaiku(BMessage& message, const char* name, const String& text)
{
    auto utf8 = text.utf8();
    // CString::data() is char8_t in this WebKit; BMessage takes char.
    const char* characters = utf8.legacyCStringPointer();
    message.AddString(name, characters ? characters : "");
}

// Converted values may be cyclic; anything deeper than this becomes null.
constexpr unsigned maximumJSONDepthHaiku = 256;

Ref<JSON::Value> jsonFromAPIHaiku(API::Object* object, unsigned depth)
{
    if (!object || depth > maximumJSONDepthHaiku)
        return JSON::Value::null();
    switch (object->type()) {
    case API::Object::Type::String:
        return JSON::Value::create(downcast<API::String>(*object).string());
    case API::Object::Type::Boolean:
        return JSON::Value::create(downcast<API::Boolean>(*object).value());
    case API::Object::Type::Double: {
        double value = downcast<API::Double>(*object).value();
        return std::isfinite(value) ? JSON::Value::create(value) : JSON::Value::null();
    }
    case API::Object::Type::UInt64:
        return JSON::Value::create(static_cast<double>(downcast<API::UInt64>(*object).value()));
    case API::Object::Type::Int64:
        return JSON::Value::create(static_cast<double>(downcast<API::Int64>(*object).value()));
    case API::Object::Type::Array: {
        auto array = JSON::Array::create();
        for (auto& element : downcast<API::Array>(*object).elements())
            array->pushValue(jsonFromAPIHaiku(element.get(), depth + 1));
        return array;
    }
    case API::Object::Type::Dictionary: {
        auto result = JSON::Object::create();
        for (auto& [key, value] : downcast<API::Dictionary>(*object).map())
            result->setValue(key, jsonFromAPIHaiku(value.get(), depth + 1));
        return result;
    }
    default:
        return JSON::Value::null();
    }
}

// Adds a script value as valueField/typeField: "string" (exact), "json" or
// "undefined". See B_WEBKIT_SCRIPT_MESSAGE and B_WEBKIT_JAVASCRIPT_RESULT.
void addScriptValueHaiku(BMessage& message, JavaScriptEvaluationResult&& value, const char* valueField, const char* typeField)
{
    auto root = value.map().find(value.root());
    if (root != value.map().end()) {
        if (auto* string = std::get_if<String>(&root->value)) {
            addStringHaiku(message, valueField, *string);
            message.AddString(typeField, "string");
            return;
        }
        if (auto* empty = std::get_if<JavaScriptEvaluationResult::EmptyType>(&root->value)) {
            bool isNull = *empty == JavaScriptEvaluationResult::EmptyType::Null;
            message.AddString(valueField, isNull ? "null" : "");
            message.AddString(typeField, isNull ? "json" : "undefined");
            return;
        }
    }
    RefPtr object = value.toAPI();
    addStringHaiku(message, valueField, jsonFromAPIHaiku(object.get(), 0)->toJSONString());
    message.AddString(typeField, "json");
}

// Replies the host owes to pages, by token. Main thread only.
HashMap<uint64_t, ScriptReplyHaiku>& pendingScriptRepliesHaiku()
{
    static NeverDestroyed<HashMap<uint64_t, ScriptReplyHaiku>> replies;
    return replies.get();
}

class ScriptMessageClientHaiku final : public WebScriptMessageHandler::Client {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ScriptMessageClientHaiku);
public:
    ScriptMessageClientHaiku(const String& name, BMessenger target)
        : m_name(name), m_target(target) { }

    void didPostMessage(WebPageProxy& page, FrameInfoData&& frame, API::ContentWorld&, JavaScriptEvaluationResult&& body, ScriptReplyHaiku&& completion) override
    {
        static uint64_t lastToken;
        const uint64_t token = ++lastToken;
        BMessage message(B_WEBKIT_SCRIPT_MESSAGE);
        message.AddMessenger("view", viewOfPageHaiku(page));
        addStringHaiku(message, "name", m_name);
        addScriptValueHaiku(message, WTF::move(body), "body", "body_type");
        message.AddUInt64("reply_token", token);
        message.AddBool("main_frame", frame.isMainFrame);
        addStringHaiku(message, "frame_url", frame.request.url().string());
        addStringHaiku(message, "origin", frame.securityOrigin.isOpaque() ? "null"_s : frame.securityOrigin.toString());
        pendingScriptRepliesHaiku().add(token, WTF::move(completion));
        if (m_target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) != B_OK) {
            if (auto reply = pendingScriptRepliesHaiku().take(token))
                reply(std::unexpected(String("The script message handler is gone"_s)));
        }
    }

private:
    const String m_name;
    const BMessenger m_target;
};

// URL scheme requests the host has yet to answer, by identifier. Main thread only.
HashMap<uint64_t, Ref<WebURLSchemeTask>>& pendingSchemeTasksHaiku()
{
    static NeverDestroyed<HashMap<uint64_t, Ref<WebURLSchemeTask>>> tasks;
    return tasks.get();
}

std::optional<uint64_t> takeSchemeTaskHaiku(WebURLSchemeTask& task)
{
    for (auto& [identifier, pending] : pendingSchemeTasksHaiku()) {
        if (pending.ptr() == &task) {
            uint64_t found = identifier;
            pendingSchemeTasksHaiku().remove(found);
            return found;
        }
    }
    return std::nullopt;
}

class URLSchemeHandlerHaiku final : public WebURLSchemeHandler {
public:
    static Ref<URLSchemeHandlerHaiku> create(const String& scheme, BMessenger target)
    {
        return adoptRef(*new URLSchemeHandlerHaiku(scheme, target));
    }

private:
    URLSchemeHandlerHaiku(const String& scheme, BMessenger target)
        : m_scheme(scheme), m_target(target) { }

    void platformStartTask(WebPageProxy& page, WebURLSchemeTask& task) final
    {
        static uint64_t lastIdentifier;
        const uint64_t identifier = ++lastIdentifier;
        auto request = task.request();
        BMessage message(B_WEBKIT_URL_SCHEME_REQUEST);
        message.AddMessenger("view", viewOfPageHaiku(page));
        message.AddUInt64("identifier", identifier);
        addStringHaiku(message, "scheme", m_scheme);
        addStringHaiku(message, "url", request.url().string());
        addStringHaiku(message, "method", request.httpMethod());
        for (auto& header : request.httpHeaderFields()) {
            addStringHaiku(message, "header_name", header.key);
            addStringHaiku(message, "header_value", header.value);
        }
        if (RefPtr body = request.httpBody()) {
            auto data = body->flatten();
            if (!data.isEmpty())
                message.AddData("body", B_RAW_TYPE, data.span().data(), data.size());
        }
        message.AddBool("main_frame", task.frameInfo().isMainFrame());
        pendingSchemeTasksHaiku().add(identifier, Ref { task });
        if (m_target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) != B_OK) {
            pendingSchemeTasksHaiku().remove(identifier);
            task.didComplete(ResourceError("WebKitURLSchemeHandler"_s, 0, request.url(), "The URL scheme handler is gone"_s));
        }
    }

    void platformStopTask(WebPageProxy& page, WebURLSchemeTask& task) final
    {
        auto identifier = takeSchemeTaskHaiku(task);
        if (!identifier)
            return;
        BMessage message(B_WEBKIT_URL_SCHEME_REQUEST_STOPPED);
        message.AddMessenger("view", viewOfPageHaiku(page));
        message.AddUInt64("identifier", *identifier);
        m_target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }

    void platformTaskCompleted(WebURLSchemeTask& task) final
    {
        takeSchemeTaskHaiku(task);
    }

    const String m_scheme;
    const BMessenger m_target;
};

struct BeforeUnloadBehaviorHaiku {
    uint32 behavior { B_WEBKIT_BEFORE_UNLOAD_PROMPT };
    BMessenger target;
};

// By page, which outlives no identifier: identifiers are never reused. Main thread only.
HashMap<WebPageProxyIdentifier, BeforeUnloadBehaviorHaiku>& beforeUnloadBehaviorsHaiku()
{
    static NeverDestroyed<HashMap<WebPageProxyIdentifier, BeforeUnloadBehaviorHaiku>> behaviors;
    return behaviors.get();
}

} // namespace

bool WebKit::answerBeforeUnloadHaiku(WebPageProxy& page, Function<void(bool)>& completion)
{
    auto found = beforeUnloadBehaviorsHaiku().find(page.identifier());
    if (found == beforeUnloadBehaviorsHaiku().end() || found->value.behavior == B_WEBKIT_BEFORE_UNLOAD_PROMPT)
        return false;
    bool leave = found->value.behavior == B_WEBKIT_BEFORE_UNLOAD_PROCEED;
    if (!leave) {
        BMessage message(B_WEBKIT_BEFORE_UNLOAD_PREVENTED);
        message.AddMessenger("view", viewOfPageHaiku(page));
        found->value.target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }
    completion(leave);
    return true;
}

void BWebKitView::SetBeforeUnloadBehavior(uint32 behavior, BMessenger target)
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, behavior, target] {
        if (auto* page = pageHaiku(state))
            beforeUnloadBehaviorsHaiku().set(page->identifier(), BeforeUnloadBehaviorHaiku { behavior, target });
    });
}

void BWebKitView::SetZoomFactor(double factor)
{
    if (m_status != B_OK || !std::isfinite(factor) || factor <= 0)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, factor] {
        if (auto* page = pageHaiku(state))
            page->setPageZoomFactor(factor);
    });
}

void BWebKitView::AddUserScript(const char* source, bool atDocumentStart, bool allFrames)
{
    if (m_status != B_OK || !source)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, source = copyTextHaiku(source), atDocumentStart, allFrames] {
        auto* page = pageHaiku(state);
        if (!page)
            return;
        WebCore::UserScript script { stringHaiku(source), URL { }, { }, { },
            atDocumentStart ? UserScriptInjectionTime::DocumentStart : UserScriptInjectionTime::DocumentEnd,
            allFrames ? UserContentInjectedFrames::InjectInAllFrames : UserContentInjectedFrames::InjectInTopFrameOnly };
        Ref userScript = API::UserScript::create(WTF::move(script), API::ContentWorld::pageContentWorldSingleton());
        page->configuration().userContentController().addUserScript(userScript.get(), InjectUserScriptImmediately::No);
    });
}

void BWebKitView::RemoveAllUserScripts()
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state] {
        if (auto* page = pageHaiku(state))
            page->configuration().userContentController().removeAllUserScripts(API::ContentWorld::pageContentWorldSingleton());
    });
}

void BWebKitView::AddScriptMessageHandler(const char* name, BMessenger target)
{
    if (m_status != B_OK || !name || !*name)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, name = copyTextHaiku(name), target] {
        auto* page = pageHaiku(state);
        if (!page)
            return;
        String handlerName = stringHaiku(name);
        Ref handler = WebScriptMessageHandler::create(makeUnique<ScriptMessageClientHaiku>(handlerName, target),
            handlerName, API::ContentWorld::pageContentWorldSingleton());
        page->configuration().userContentController().addUserScriptMessageHandler(handler.get());
    });
}

void BWebKitView::RemoveScriptMessageHandler(const char* name)
{
    if (m_status != B_OK || !name)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, name = copyTextHaiku(name)] {
        if (auto* page = pageHaiku(state))
            page->configuration().userContentController().removeUserMessageHandlerForName(stringHaiku(name), API::ContentWorld::pageContentWorldSingleton());
    });
}

void BWebKitView::ReplyToScriptMessage(uint64 replyToken, const char* reply, const char* error)
{
    std::optional<std::string> errorText;
    if (error)
        errorText = error;
    WTF::RunLoop::mainSingleton().dispatch([replyToken, reply = copyTextHaiku(reply), errorText = std::move(errorText)] {
        auto completion = pendingScriptRepliesHaiku().take(replyToken);
        if (!completion)
            return;
        if (errorText) {
            completion(std::unexpected(stringHaiku(*errorText)));
            return;
        }
        auto result = JavaScriptEvaluationResult::extract(API::String::create(stringHaiku(reply)).ptr());
        if (!result) {
            completion(std::unexpected(String("The reply could not be passed to the page"_s)));
            return;
        }
        completion(WTF::move(*result));
    });
}

void BWebKitView::EvaluateJavaScript(const char* source, BMessenger reply, uint64 identifier, bool asAsyncFunction)
{
    auto fail = [&](const char* error) {
        BMessage message(B_WEBKIT_JAVASCRIPT_RESULT);
        message.AddUInt64("identifier", identifier);
        message.AddString("error", error);
        reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    };
    if (m_status != B_OK) {
        fail("The view has no page");
        return;
    }
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, source = copyTextHaiku(source), reply, identifier, asAsyncFunction] {
        BMessage message(B_WEBKIT_JAVASCRIPT_RESULT);
        message.AddMessenger("view", viewOfStateHaiku(state));
        message.AddUInt64("identifier", identifier);
        auto* page = pageHaiku(state);
        auto string = page ? IPC::TransferString::create(stringHaiku(source)) : std::nullopt;
        if (!string) {
            message.AddString("error", page ? "Out of memory" : "The view is closed");
            reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
            return;
        }
        WebKit::RunJavaScriptParameters parameters {
            WTF::move(*string),
            JSC::SourceTaintedOrigin::Untainted,
            URL { },
            asAsyncFunction ? RunAsAsyncFunction::Yes : RunAsAsyncFunction::No,
            // An async function needs an (empty) argument list: ScriptController
            // iterates it unconditionally, as callAsyncJavaScript passes one.
            asAsyncFunction ? std::optional { Vector<std::pair<String, JavaScriptEvaluationResult>> { } } : std::nullopt,
            ForceUserGesture::Yes,
            RemoveTransientActivation::Yes
        };
        page->runJavaScriptInMainFrame(WTF::move(parameters), true, [message = WTF::move(message), reply](auto&& result) mutable {
            if (result)
                addScriptValueHaiku(message, WTF::move(*result), "result", "result_type");
            else if (!result.error())
                message.AddString("error", "The script returned a value that cannot be passed back");
            else {
                auto& details = *result.error();
                StringBuilder builder;
                if (!details.sourceURL.isEmpty()) {
                    builder.append(details.sourceURL);
                    if (details.lineNumber > 0)
                        builder.append(':', details.lineNumber);
                    builder.append(": "_s);
                }
                builder.append(details.message);
                addStringHaiku(message, "error", builder.toString());
            }
            reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
        });
    });
}

void BWebKitView::SetCustomUserAgent(const char* userAgent)
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, userAgent = copyTextHaiku(userAgent)] {
        if (auto* page = pageHaiku(state))
            page->setCustomUserAgent(stringHaiku(userAgent));
    });
}

void BWebKitView::SetURLSchemeHandler(const char* scheme, BMessenger target, uint32 flags)
{
    if (m_status != B_OK || !scheme || !*scheme)
        return;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, scheme = copyTextHaiku(scheme), target, flags] {
        auto* page = pageHaiku(state);
        if (!page)
            return;
        auto canonical = WTF::URLParser::maybeCanonicalizeScheme(stringHaiku(scheme));
        if (!canonical || WTF::URLParser::isSpecialScheme(*canonical) || page->urlSchemeHandlerForScheme(*canonical))
            return;
        String name = *canonical;
        auto& pool = page->configuration().processPool();
        if (flags & B_WEBKIT_SCHEME_SECURE)
            pool.registerURLSchemeAsSecure(name);
        if (flags & B_WEBKIT_SCHEME_CORS_ENABLED)
            pool.registerURLSchemeAsCORSEnabled(name);
        if (flags & B_WEBKIT_SCHEME_BYPASS_CSP)
            pool.registerURLSchemeAsBypassingContentSecurityPolicy(name);
        if (flags & B_WEBKIT_SCHEME_LOCAL)
            pool.registerURLSchemeAsLocal(name);
        page->setURLSchemeHandlerForScheme(URLSchemeHandlerHaiku::create(name, target), name);
    });
}

void BWebKitView::RespondToURLSchemeRequest(uint64 identifier, int32 status, const char* mimeType,
    const BMessage* headers, const void* data, size_t size)
{
    std::vector<std::pair<std::string, std::string>> headerList;
    if (headers) {
        const char* name;
        const char* value;
        for (int32 index = 0; headers->FindString("name", index, &name) == B_OK
            && headers->FindString("value", index, &value) == B_OK; ++index)
            headerList.emplace_back(name, value);
    }
    std::vector<uint8_t> body;
    if (data && size)
        body.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    WTF::RunLoop::mainSingleton().dispatch([identifier, status, mimeType = copyTextHaiku(mimeType),
        headerList = std::move(headerList), body = std::move(body)] {
        auto task = pendingSchemeTasksHaiku().take(identifier);
        if (!task)
            return;
        URL url = task->request().url();
        String mediaType = stringHaiku(mimeType);
        String type = extractMIMETypeFromMediaType(mediaType);
        if (type.isEmpty())
            type = MIMETypeRegistry::mimeTypeForPath(url.path());
        ResourceResponse response(URL { url }, String { type }, static_cast<long long>(body.size()), extractCharsetFromMediaType(mediaType).toString());
        response.setHTTPStatusCode(status);
        response.setHTTPStatusText(status >= 200 && status < 300 ? String { "OK"_s } : String { });
        bool hasContentType = false;
        for (auto& [name, value] : headerList) {
            String headerName = stringHaiku(name);
            hasContentType |= equalLettersIgnoringASCIICase(headerName, "content-type"_s);
            response.setHTTPHeaderField(headerName, stringHaiku(value));
        }
        if (!hasContentType && !type.isEmpty())
            response.setHTTPHeaderField(HTTPHeaderName::ContentType, mediaType.isEmpty() ? type : mediaType);
        task->didReceiveResponse(WTF::move(response));
        if (!body.empty())
            task->didReceiveData(SharedBuffer::create(std::span<const uint8_t> { body.data(), body.size() }));
        task->didComplete({ });
    });
}

void BWebKitView::FailURLSchemeRequest(uint64 identifier, const char* description)
{
    WTF::RunLoop::mainSingleton().dispatch([identifier, description = copyTextHaiku(description)] {
        auto task = pendingSchemeTasksHaiku().take(identifier);
        if (!task)
            return;
        task->didComplete(ResourceError("WebKitURLSchemeHandler"_s, 0, task->request().url(),
            description.empty() ? "The request failed"_s : stringHaiku(description)));
    });
}

namespace {

void addCookieHaiku(BMessage& message, const WebCore::Cookie& cookie)
{
    BMessage entry;
    addStringHaiku(entry, "name", cookie.name);
    addStringHaiku(entry, "value", cookie.value);
    addStringHaiku(entry, "domain", cookie.domain);
    addStringHaiku(entry, "path", cookie.path);
    entry.AddString("sameSite", cookie.sameSite == WebCore::Cookie::SameSitePolicy::Strict ? "strict"
        : cookie.sameSite == WebCore::Cookie::SameSitePolicy::Lax ? "lax" : "none");
    entry.AddBool("httpOnly", cookie.httpOnly);
    entry.AddBool("secure", cookie.secure);
    entry.AddBool("session", cookie.session || !cookie.expires);
    if (cookie.expires && !cookie.session)
        entry.AddDouble("expires", *cookie.expires);
    message.AddMessage("cookie", &entry);
}

void replyHaiku(BMessenger reply, uint32 what, uint64 identifier, const char* error = nullptr)
{
    BMessage message(what);
    message.AddUInt64("identifier", identifier);
    if (error)
        message.AddString("error", error);
    reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
}

} // namespace

void BWebKitView::GetCookies(const char* url, BMessenger reply, uint64 identifier)
{
    if (m_status != B_OK) {
        replyHaiku(reply, B_WEBKIT_COOKIES, identifier, "The view has no page");
        return;
    }
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, url = copyTextHaiku(url), reply, identifier] {
        auto* page = pageHaiku(state);
        if (!page) {
            replyHaiku(reply, B_WEBKIT_COOKIES, identifier, "The view is closed");
            return;
        }
        auto deliver = [reply, identifier](Vector<WebCore::Cookie>&& cookies) {
            BMessage message(B_WEBKIT_COOKIES);
            message.AddUInt64("identifier", identifier);
            for (auto& cookie : cookies)
                addCookieHaiku(message, cookie);
            reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
        };
        auto& store = page->websiteDataStore().cookieStore();
        if (url.empty())
            store.cookies(WTF::move(deliver));
        else
            store.cookiesForURL(URL { stringHaiku(url) }, WTF::move(deliver));
    });
}

void BWebKitView::SetCookie(const BMessage& fields, BMessenger reply, uint64 identifier)
{
    if (m_status != B_OK) {
        replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier, "The view has no page");
        return;
    }
    WebCore::Cookie cookie;
    cookie.name = String::fromUTF8(fields.GetString("name", ""));
    cookie.value = String::fromUTF8(fields.GetString("value", ""));
    cookie.domain = String::fromUTF8(fields.GetString("domain", ""));
    cookie.path = String::fromUTF8(fields.GetString("path", "/"));
    cookie.httpOnly = fields.GetBool("httpOnly", false);
    cookie.secure = fields.GetBool("secure", false);
    double expires;
    if (fields.FindDouble("expires", &expires) == B_OK && !fields.GetBool("session", false))
        cookie.expires = expires;
    else
        cookie.session = true;
    cookie.created = WallTime::now().secondsSinceEpoch().milliseconds();
    std::string sameSite = fields.GetString("sameSite", "none");
    cookie.sameSite = sameSite == "strict" ? WebCore::Cookie::SameSitePolicy::Strict
        : sameSite == "lax" ? WebCore::Cookie::SameSitePolicy::Lax : WebCore::Cookie::SameSitePolicy::None;
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, cookie = WTF::move(cookie), reply, identifier]() mutable {
        auto* page = pageHaiku(state);
        if (!page) {
            replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier, "The view is closed");
            return;
        }
        page->websiteDataStore().cookieStore().setCookies({ WTF::move(cookie) }, [reply, identifier] {
            replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier);
        });
    });
}

void BWebKitView::DeleteCookies(const char* url, const char* name, BMessenger reply, uint64 identifier)
{
    if (m_status != B_OK) {
        replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier, "The view has no page");
        return;
    }
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, url = copyTextHaiku(url), name = copyTextHaiku(name), reply, identifier] {
        auto* page = pageHaiku(state);
        if (!page) {
            replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier, "The view is closed");
            return;
        }
        Ref<API::HTTPCookieStore> store = page->websiteDataStore().cookieStore();
        auto remove = [store, name = stringHaiku(name), reply, identifier](Vector<WebCore::Cookie>&& cookies) {
            Vector<WebCore::Cookie> doomed;
            for (auto& cookie : cookies) {
                if (name.isEmpty() || cookie.name == name)
                    doomed.append(cookie);
            }
            if (doomed.isEmpty()) {
                replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier);
                return;
            }
            // Answer once, after the last deletion.
            auto remaining = std::make_shared<size_t>(doomed.size());
            for (auto& cookie : doomed) {
                store->deleteCookie(cookie, [remaining, reply, identifier] {
                    if (!--*remaining)
                        replyHaiku(reply, B_WEBKIT_COOKIES_CHANGED, identifier);
                });
            }
        };
        if (url.empty())
            store->cookies(WTF::move(remove));
        else
            store->cookiesForURL(URL { stringHaiku(url) }, WTF::move(remove));
    });
}

void BWebKitView::RemoveWebsiteData(uint32 types, BMessenger reply, uint64 identifier)
{
    if (m_status != B_OK) {
        replyHaiku(reply, B_WEBKIT_WEBSITE_DATA_REMOVED, identifier, "The view has no page");
        return;
    }
    WTF::RunLoop::mainSingleton().dispatch([state = m_state, types, reply, identifier] {
        auto* page = pageHaiku(state);
        if (!page) {
            replyHaiku(reply, B_WEBKIT_WEBSITE_DATA_REMOVED, identifier, "The view is closed");
            return;
        }
        OptionSet<WebsiteDataType> data;
        if (types & B_WEBKIT_DATA_COOKIES)
            data.add(WebsiteDataType::Cookies);
        if (types & B_WEBKIT_DATA_CACHE)
            data.add({ WebsiteDataType::DiskCache, WebsiteDataType::MemoryCache });
        if (types & B_WEBKIT_DATA_LOCAL_STORAGE)
            data.add({ WebsiteDataType::LocalStorage, WebsiteDataType::SessionStorage });
        if (types & B_WEBKIT_DATA_INDEXED_DB)
            data.add(WebsiteDataType::IndexedDBDatabases);
        if (types & B_WEBKIT_DATA_SERVICE_WORKERS)
            data.add({ WebsiteDataType::ServiceWorkerRegistrations, WebsiteDataType::DOMCache });
        page->websiteDataStore().removeData(data, WallTime::fromRawSeconds(0), [reply, identifier] {
            replyHaiku(reply, B_WEBKIT_WEBSITE_DATA_REMOVED, identifier);
        });
    });
}
