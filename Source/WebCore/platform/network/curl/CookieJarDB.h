/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CookieChange.h"
#include "CookieJar.h"
#include "SQLiteDatabase.h"
#include "SQLiteStatement.h"
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/RunLoop.h>
#include <wtf/SharedTask.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WallTime.h>
#include <wtf/text/StringHash.h>

namespace WebCore {

enum class CookieAcceptPolicy {
    Always,
    Never,
    OnlyFromMainDocumentDomain,
    ExclusivelyFromMainDocumentDomain
};

class CookieJarDB {
    WTF_MAKE_TZONE_ALLOCATED(CookieJarDB);
    WTF_MAKE_NONCOPYABLE(CookieJarDB);

public:
    enum class Source : uint8_t {
        Network,
        Script
    };

    WEBCORE_EXPORT void open();
    bool isEnabled() const;

    void setAcceptPolicy(CookieAcceptPolicy policy) { m_acceptPolicy = policy; scheduleCookieExpiration(); }
    CookieAcceptPolicy acceptPolicy() const { return m_acceptPolicy; }

    WEBCORE_EXPORT HashSet<String> allDomains();
    WEBCORE_EXPORT std::optional<Vector<Cookie>> searchCookies(const URL& firstParty, const URL& requestUrl, const std::optional<bool>& httpOnly, const std::optional<bool>& secure, const std::optional<bool>& session);
    WEBCORE_EXPORT Vector<Cookie> getAllCookies();
    WEBCORE_EXPORT bool setCookie(const URL& firstParty, const URL&, const String& cookie, Source, std::optional<Seconds> cappedLifetime = std::nullopt);
    WEBCORE_EXPORT bool setCookie(const Cookie&);
    // Returns the row actually stored, including preserved creation time and
    // normalized expiry. Successful expiry/deletion returns an empty cookie.
    WEBCORE_EXPORT bool setCookieWithResult(const Cookie&, std::optional<Cookie>&);

    WEBCORE_EXPORT bool deleteCookie(const Cookie&);
    WEBCORE_EXPORT bool deleteCookie(const String& url, const String& name);
    bool deleteCookies(const String& url);
    WEBCORE_EXPORT bool deleteCookiesForHostname(const String& hostname, IncludeHttpOnlyCookies);
    WEBCORE_EXPORT bool deleteAllCookies();
    WEBCORE_EXPORT bool deleteCookiesModifiedSince(WallTime);
    WEBCORE_EXPORT bool deleteExpiredCookies();

    using ChangeHandler = SharedTask<void(Vector<CookieChange>&&)>;
    // A replacement is delivered as one ordered batch, after the SQL commit.
    WEBCORE_EXPORT void setCookieChangeHandler(RefPtr<ChangeHandler>&&);

    WEBCORE_EXPORT CookieJarDB(const String& databasePath);
    WEBCORE_EXPORT ~CookieJarDB();

private:
    CookieAcceptPolicy m_acceptPolicy { CookieAcceptPolicy::Always };
    String m_databasePath;

    bool m_detectedDatabaseCorruption { false };

    bool isOnMemory() const { return m_databasePath == ":memory:"_s; };

    bool openDatabase();
    void closeDatabase();

    bool checkSQLiteReturnCode(int);
    void flagDatabaseCorruption();
    bool checkDatabaseCorruptionAndRemoveIfNeeded();
    String getCorruptionMarkerPath() const;

    bool checkDatabaseValidity();
    void deleteAllDatabaseFiles();

    bool verifySchemaVersion();
    void deleteAllTables();

    bool createPrepareStatement(ASCIILiteral);
    SQLiteStatement& preparedStatement(const String&);
    bool executeSQLStatement(std::unique_ptr<SQLiteStatement>&&);

    bool deleteCookieInternal(const String& name, const String& domain, const String& path);
    bool readCookie(const Cookie&, std::optional<Cookie>&);
    bool deleteCookiesMatching(ASCIILiteral selectSQL, ASCIILiteral deleteSQL, Function<bool(SQLiteStatement&)>&&, CookieChangeCause);
    void notifyCookieChanges(Vector<CookieChange>&&);
    void scheduleCookieExpiration();
    void cookieExpirationTimerFired();
    RefPtr<ChangeHandler> m_cookieChangeHandler;
    std::unique_ptr<RunLoop::Timer> m_cookieExpirationTimer;

    bool hasHttpOnlyCookie(const String& name, const String& domain, const String& path);
    bool canAcceptCookie(const Cookie&, const URL& firstParty, const URL&, CookieJarDB::Source);
    bool checkCookieAcceptPolicy(const URL& firstParty, const URL&);
    bool hasCookies(const URL&);

    SQLiteDatabase m_database;
    HashMap<String, std::unique_ptr<SQLiteStatement>> m_statements;
};

} // namespace WebCore
