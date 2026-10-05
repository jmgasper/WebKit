/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <SupportDefs.h>

// Embedding interfaces of BWebKitView for applications that host their own
// pages: user scripts, script messages, script evaluation and URL schemes.
// Aurora, the Electron-compatible runtime, is built on these; they suit any
// host that needs its pages to talk to native code.
//
// Every BWebKitView call below is queued to the WebKit main thread in the
// order it is made, after the view's own creation. Configure scripts, message
// handlers and schemes before the first LoadURL() so the first document sees
// them. Pages the view opens (window.open) share its scripts and handlers.

enum : uint32 {
    // A page called window.webkit.messageHandlers.<name>.postMessage(value).
    // Fields: "view" (messenger), "name" (handler name), "body" (string),
    // "body_type" ("string" when the page posted a string, which "body" then
    // holds exactly; otherwise "json" and "body" is the value as JSON text),
    // "reply_token" (uint64), "main_frame" (bool), "frame_url" and "origin"
    // (strings). WebKit drops null members of posted arrays and objects, so
    // post JSON text when the data must arrive exactly.
    // postMessage() returns a promise. Answer every message exactly once with
    // BWebKitView::ReplyToScriptMessage(); until then the promise is pending.
    B_WEBKIT_SCRIPT_MESSAGE = 'wsmg',
    // Answers EvaluateJavaScript(). Fields: "view", "identifier" (uint64),
    // and either "result" (string) with "result_type" ("string": the script's
    // value was that string; "json": its JSON text; "undefined": no value), or
    // "error" (string: the exception, with source position when known).
    B_WEBKIT_JAVASCRIPT_RESULT = 'wjsr',
    // A request for a URL of a scheme given to SetURLSchemeHandler(). Fields:
    // "view", "identifier" (uint64), "scheme", "url", "method" (strings),
    // repeated "header_name"/"header_value" pairs, optional "body"
    // (B_RAW_TYPE), "main_frame" (bool). Answer with
    // RespondToURLSchemeRequest() or FailURLSchemeRequest().
    B_WEBKIT_URL_SCHEME_REQUEST = 'wusr',
    // The engine no longer wants the answer to a request, which was for
    // example cancelled by navigation. Fields: "view", "identifier".
    B_WEBKIT_URL_SCHEME_REQUEST_STOPPED = 'wuss',
    // A beforeunload handler kept the page, and the view's behavior is
    // B_WEBKIT_BEFORE_UNLOAD_CANCEL, so no question was asked. Fields: "view".
    B_WEBKIT_BEFORE_UNLOAD_PREVENTED = 'wbup',
    // Answers GetCookies(). Fields: "identifier" (uint64), "error" (string)
    // on failure, else repeated "cookie" messages: "name", "value", "domain",
    // "path", "sameSite" ("none", "lax", "strict") strings; "httpOnly",
    // "secure", "session" bools; "expires" (double, milliseconds since the
    // epoch) unless it is a session cookie.
    B_WEBKIT_COOKIES = 'wcok',
    // Answers SetCookie() and DeleteCookies(): "identifier", "error" on failure.
    B_WEBKIT_COOKIES_CHANGED = 'wcoc',
    // Answers RemoveWebsiteData(): "identifier".
    B_WEBKIT_WEBSITE_DATA_REMOVED = 'wdrm'
};

// RemoveWebsiteData(): what to remove.
enum : uint32 {
    B_WEBKIT_DATA_COOKIES = 1 << 0,
    B_WEBKIT_DATA_CACHE = 1 << 1,           // disk and memory cache
    B_WEBKIT_DATA_LOCAL_STORAGE = 1 << 2,   // local and session storage
    B_WEBKIT_DATA_INDEXED_DB = 1 << 3,
    B_WEBKIT_DATA_SERVICE_WORKERS = 1 << 4, // registrations and DOM caches
    B_WEBKIT_DATA_ALL = 0xffffffff
};

// SetBeforeUnloadBehavior(): what happens when a page's beforeunload handler
// asks to stay, on close, reload or navigation.
enum : uint32 {
    // Ask the user, as a browser does (the default).
    B_WEBKIT_BEFORE_UNLOAD_PROMPT = 0,
    // Stay without asking and send B_WEBKIT_BEFORE_UNLOAD_PREVENTED, as
    // Electron does; the host may then close or reload regardless.
    B_WEBKIT_BEFORE_UNLOAD_CANCEL = 1,
    // Leave without asking: the handlers' answer is ignored.
    B_WEBKIT_BEFORE_UNLOAD_PROCEED = 2
};

// Flags of SetURLSchemeHandler(): how pages of the scheme are treated.
enum : uint32 {
    // Pages are a secure context (crypto.subtle, service workers, storage).
    B_WEBKIT_SCHEME_SECURE = 1 << 0,
    // Other origins may fetch the scheme's URLs under CORS.
    B_WEBKIT_SCHEME_CORS_ENABLED = 1 << 1,
    // Pages of the scheme ignore their own Content-Security-Policy.
    B_WEBKIT_SCHEME_BYPASS_CSP = 1 << 2,
    // The scheme is local: only local pages may load its URLs.
    B_WEBKIT_SCHEME_LOCAL = 1 << 3
};
