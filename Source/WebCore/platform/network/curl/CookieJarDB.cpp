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

#include "config.h"
#include "CookieJarDB.h"
#include "CookieJar.h"

#include "CookieUtil.h"
#include "Logging.h"
#include "PublicSuffixStore.h"
#include "RegistrableDomain.h"
#include "SQLiteFileSystem.h"
#include "SQLiteTransaction.h"
#include <algorithm>
#include <cmath>
#include <wtf/DateMath.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CookieJarDB);

// At least 50 cookies per domain (RFC6265 6.1. Limits)
#define MAX_COOKIE_PER_DOMAIN 80

#define CREATE_COOKIE_TABLE_SQL \
    "CREATE TABLE IF NOT EXISTS Cookie ("\
    "  name TEXT NOT NULL,"\
    "  value TEXT,"\
    "  domain TEXT NOT NULL,"\
    "  path TEXT NOT NULL,"\
    "  expires INTEGER NOT NULL,"\
    "  size INTEGER NOT NULL,"\
    "  session INTEGER NOT NULL,"\
    "  httponly INTEGER NOT NULL DEFAULT 0,"\
    "  secure INTEGER NOT NULL DEFAULT 0,"\
    "  created REAL NOT NULL DEFAULT 0,"\
    "  samesite INTEGER NOT NULL DEFAULT 0,"\
    "  lastupdated INTEGER NOT NULL DEFAULT CURRENT_TIMESTAMP, "\
    "  UNIQUE(name, domain, path));"_s
#define CREATE_DOMAIN_INDEX_SQL \
    "CREATE INDEX IF NOT EXISTS domain_index ON Cookie(domain);"_s
#define CREATE_PATH_INDEX_SQL \
    "CREATE INDEX IF NOT EXISTS path_index ON Cookie(path);"_s
#define SELECT_ALL_DOMAINS_SQL \
    "SELECT DISTINCT domain FROM Cookie;"_s
#define CHECK_EXISTS_COOKIE_SQL \
    "SELECT domain FROM Cookie WHERE ((domain = ?) OR (domain GLOB ?));"_s
#define CHECK_EXISTS_HTTPONLY_COOKIE_SQL \
    "SELECT name FROM Cookie WHERE (name = ?) AND (domain = ?) AND (path = ?) AND (httponly = 1);"_s
#define SET_COOKIE_SQL \
    "INSERT INTO Cookie (name, value, domain, path, expires, size, session, httponly, secure, created, samesite) "\
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "\
    "ON CONFLICT(name, domain, path) DO UPDATE SET value=excluded.value, expires=excluded.expires, "\
    "size=excluded.size, session=excluded.session, httponly=excluded.httponly, secure=excluded.secure, "\
    "samesite=excluded.samesite, created=COALESCE(NULLIF(Cookie.created, 0), excluded.created), lastupdated=CURRENT_TIMESTAMP;"_s
#define SELECT_COOKIE_COLUMNS_SQL \
    "SELECT name, value, domain, path, expires, httponly, secure, session, created, samesite FROM Cookie "
#define DELETE_COOKIE_BY_NAME_DOMAIN_PATH_SQL \
    "DELETE FROM Cookie WHERE name = ? AND domain = ? AND path = ?;"_s
#define DELETE_COOKIE_BY_NAME_DOMAIN_SQL \
    "DELETE FROM Cookie WHERE name = ? AND domain = ?;"_s
#define DELETE_ALL_SESSION_COOKIE_SQL \
    "DELETE FROM Cookie WHERE session = 1;"_s
#define DELETE_COOKIES_BY_DOMAIN_SQL \
    "DELETE FROM Cookie WHERE domain = ? ;"_s
#define DELETE_COOKIES_BY_DOMAIN_EXCEPT_HTTP_ONLY_SQL \
    "DELETE FROM Cookie WHERE (domain = ?) AND (httponly = 0);"_s
#define DELETE_ALL_COOKIE_SQL \
    "DELETE FROM Cookie;"_s


// If the database schema is updated:
// - Increment schemaVersion
// - Add upgrade logic in verifySchemaVersion to migrate databases from the previous schema version
static constexpr int schemaVersion = 2;


CookieJarDB::CookieJarDB(const String& databasePath)
    : m_databasePath(databasePath)
{
}

CookieJarDB::~CookieJarDB()
{
    m_cookieExpirationTimer = nullptr;
    closeDatabase();
    m_database.setDidBeginCheckedPtrDeletion();
}

void CookieJarDB::open()
{
    if (!m_database.isOpen()) {
        checkDatabaseCorruptionAndRemoveIfNeeded();
        openDatabase();
    }
}

bool CookieJarDB::openDatabase()
{
    if (m_database.isOpen())
        return true;

    bool existsDatabaseFile = false;
    if (!isOnMemory())
        existsDatabaseFile = SQLiteFileSystem::ensureDatabaseFileExists(m_databasePath, false);

    if (existsDatabaseFile) {
        if (m_database.open(m_databasePath)) {
            if (!checkDatabaseValidity()) {
                // delete database and try to re-create again
                LOG_ERROR("Cookie database validity check failed, attempting to recreate the database");
                m_database.close();
                deleteAllDatabaseFiles();
                existsDatabaseFile = false;
            }
        } else {
            LOG_ERROR("Failed to open cookie database: %s, attempting to recreate the database", m_databasePath.utf8().legacyCStringPointer());
            deleteAllDatabaseFiles();
            existsDatabaseFile = false;
        }
    }

    if (!existsDatabaseFile) {
        if (!isOnMemory() && !FileSystem::makeAllDirectories(FileSystem::parentPath(m_databasePath)))
            LOG_ERROR("Unable to create the Cookie Database path %s", m_databasePath.utf8().legacyCStringPointer());

        m_database.open(m_databasePath);
    }

    if (!m_database.isOpen())
        return false;

    if (!isOnMemory() && !m_database.turnOnIncrementalAutoVacuum())
        LOG_ERROR("Unable to turn on incremental auto-vacuum (%d %s)", m_database.lastError(), m_database.lastErrorMsg());

    if (!verifySchemaVersion()) {
        m_database.close();
        return false;
    }

    if (!existsDatabaseFile || !m_database.tableExists("Cookie"_s)) {
        bool ok = executeSQLStatement(m_database.prepareStatement(CREATE_COOKIE_TABLE_SQL))
            && executeSQLStatement(m_database.prepareStatement(CREATE_DOMAIN_INDEX_SQL))
            && executeSQLStatement(m_database.prepareStatement(CREATE_PATH_INDEX_SQL));

        if (!ok) {
            // give up create database at this time (all cookies on request/response are ignored)
            m_database.close();
            deleteAllDatabaseFiles();
            return false;
        }
    }

    m_database.setSynchronous(SQLiteDatabase::SyncNormal);

    // create prepared statements
    if (!createPrepareStatement(SET_COOKIE_SQL)
        || !createPrepareStatement(CHECK_EXISTS_COOKIE_SQL)
        || !createPrepareStatement(CHECK_EXISTS_HTTPONLY_COOKIE_SQL)
        || !createPrepareStatement(DELETE_COOKIE_BY_NAME_DOMAIN_PATH_SQL)
        || !createPrepareStatement(DELETE_COOKIE_BY_NAME_DOMAIN_SQL)
        || !createPrepareStatement(DELETE_COOKIES_BY_DOMAIN_SQL)
        || !createPrepareStatement(DELETE_COOKIES_BY_DOMAIN_EXCEPT_HTTP_ONLY_SQL)) {
        closeDatabase();
        return false;
    }

    if (existsDatabaseFile && !executeSQLStatement(m_database.prepareStatement(DELETE_ALL_SESSION_COOKIE_SQL))) {
        closeDatabase();
        return false;
    }

    scheduleCookieExpiration();
    return true;
}

void CookieJarDB::closeDatabase()
{
    if (m_database.isOpen()) {
        m_statements.clear();
        m_database.close();
    }
}

bool CookieJarDB::verifySchemaVersion()
{
    auto statement = m_database.prepareStatement("PRAGMA user_version"_s);
    if (!statement || statement->step() != SQLITE_ROW)
        return false;
    int version = statement->columnInt(0);
    statement = nullptr;
    if (version == schemaVersion)
        return true;
    if (version < 0 || version > schemaVersion) {
        LOG_ERROR("Unsupported cookie database version: %d", version);
        return false;
    }

    bool hasCookies = m_database.tableExists("Cookie"_s);
    SQLiteTransaction transaction(m_database);
    transaction.begin();
    if (!transaction.inProgress())
        return false;
    // Version zero with an existing table is the legacy, unversioned schema.
    // New databases get the complete current schema below in openDatabase().
    if (hasCookies && version <= 1) {
        if (!m_database.executeCommand("ALTER TABLE Cookie ADD COLUMN created REAL NOT NULL DEFAULT 0"_s)
            || !m_database.executeCommand("ALTER TABLE Cookie ADD COLUMN samesite INTEGER NOT NULL DEFAULT 0"_s))
            return false;
    }
    if (!m_database.executeCommandSlow(makeString("PRAGMA user_version="_s, schemaVersion)))
        return false;
    transaction.commit();
    return !transaction.inProgress();
}

void CookieJarDB::deleteAllTables()
{
    if (!m_database.isOpen())
        return;

    m_database.clearAllTables();
}

String CookieJarDB::getCorruptionMarkerPath() const
{
    ASSERT(!isOnMemory());

    return makeString(m_databasePath, "-corrupted"_s);
}

void CookieJarDB::flagDatabaseCorruption()
{
    if (isOnMemory())
        return;

    auto handle = FileSystem::openFile(getCorruptionMarkerPath(), FileSystem::FileOpenMode::Truncate);
}

bool CookieJarDB::checkDatabaseCorruptionAndRemoveIfNeeded()
{
    if (!isOnMemory() && FileSystem::fileExists(getCorruptionMarkerPath())) {
        LOG_ERROR("Detected cookie database corruption, attempting to recreate the database");
        deleteAllDatabaseFiles();
        return true;
    }

    return false;
}

bool CookieJarDB::checkSQLiteReturnCode(int code)
{
    if (!m_detectedDatabaseCorruption) {
        switch (code) {
        case SQLITE_CORRUPT:
        case SQLITE_SCHEMA:
        case SQLITE_FORMAT:
        case SQLITE_NOTADB:
            flagDatabaseCorruption();
            m_detectedDatabaseCorruption = true;
        }
    }
    return code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW;
}

bool CookieJarDB::checkDatabaseValidity()
{
    ASSERT(m_database.isOpen());

    if (!m_database.tableExists("Cookie"_s))
        return false;

    auto integrity = m_database.prepareStatement("PRAGMA quick_check;"_s);
    if (!integrity) {
        LOG_ERROR("Failed to execute database integrity check");
        return false;
    }

    int resultCode = integrity->step();
    if (resultCode != SQLITE_ROW) {
        LOG_ERROR("Integrity quick_check step returned %d", resultCode);
        return false;
    }

    int columns = integrity->columnCount();
    if (columns != 1) {
        LOG_ERROR("Received %i columns performing integrity check, should be 1", columns);
        return false;
    }

    String resultText = integrity->columnText(0);

    if (resultText != "ok"_s) {
        LOG_ERROR("Cookie database integrity check failed - %s", resultText.ascii().data());
        return false;
    }

    return true;
}

void CookieJarDB::deleteAllDatabaseFiles()
{
    closeDatabase();
    if (isOnMemory())
        return;

    FileSystem::deleteFile(m_databasePath);
    FileSystem::deleteFile(getCorruptionMarkerPath());
    FileSystem::deleteFile(makeString(m_databasePath, "-shm"_s));
    FileSystem::deleteFile(makeString(m_databasePath, "-wal"_s));
}

bool CookieJarDB::isEnabled() const
{
    if (m_databasePath.isEmpty())
        return false;

    return (m_acceptPolicy == CookieAcceptPolicy::Always || m_acceptPolicy == CookieAcceptPolicy::OnlyFromMainDocumentDomain || m_acceptPolicy == CookieAcceptPolicy::ExclusivelyFromMainDocumentDomain);
}

bool CookieJarDB::checkCookieAcceptPolicy(const URL& firstParty, const URL& url)
{
    if (m_acceptPolicy == CookieAcceptPolicy::Always)
        return true;

    // See https://bugs.webkit.org/show_bug.cgi?id=193458#c0
    if (m_acceptPolicy != CookieAcceptPolicy::OnlyFromMainDocumentDomain && m_acceptPolicy != CookieAcceptPolicy::ExclusivelyFromMainDocumentDomain)
        return false;

    if (firstParty.host() == url.host())
        return true;

    if (RegistrableDomain(firstParty).matches(url))
        return true;

    // third-party resources can read or write cookies if they have pre-existing cookies.
    if (m_acceptPolicy == CookieAcceptPolicy::OnlyFromMainDocumentDomain && hasCookies(url))
        return true;

    return false;
}

bool CookieJarDB::hasCookies(const URL& url)
{
    String host = url.host().convertToASCIILowercase();
    if (host.isEmpty())
        return false;

    if (PublicSuffixStore::singleton().isPublicSuffix(host))
        return false;

    RegistrableDomain registrableDomain { url };
    auto& statement = preparedStatement(CHECK_EXISTS_COOKIE_SQL);

    if (CookieUtil::isIPAddress(host) || !host.contains('.') || registrableDomain.isEmpty()) {
        statement.bindText(1, host);
        statement.bindNull(2);
    } else {
        statement.bindText(1, registrableDomain.string());
        statement.bindText(2, makeString("*."_s, registrableDomain.string()));
    }

    return statement.step() == SQLITE_ROW;
}

std::optional<Vector<Cookie>> CookieJarDB::searchCookies(const URL& firstParty, const URL& requestUrl, const std::optional<bool>& httpOnly, const std::optional<bool>& secure, const std::optional<bool>& session)
{
    if (!isEnabled() || !m_database.isOpen())
        return std::nullopt;

    String requestHost = requestUrl.host().convertToASCIILowercase();
    if (requestHost.isEmpty())
        return std::nullopt;

    if (!checkCookieAcceptPolicy(firstParty, requestUrl))
        return std::nullopt;

    String requestPath = requestUrl.path().toString();
    if (requestPath.isEmpty())
        requestPath = "/"_s;

    RegistrableDomain registrableDomain { requestUrl };

    auto pstmt = m_database.prepareStatement("SELECT name, value, domain, path, expires, httponly, secure, session, created, samesite FROM Cookie WHERE "\
        "(NOT ((session = 0) AND (expires <= ?)))"
        "AND (httponly = COALESCE(NULLIF(?, -1), httponly)) "\
        "AND (secure = COALESCE(NULLIF(?, -1), secure)) "\
        "AND (session = COALESCE(NULLIF(?, -1), session)) "\
        "AND ((domain = ?) OR (domain GLOB ?)) "\
        "ORDER BY length(path) DESC, created, rowid"_s);
    if (!pstmt)
        return std::nullopt;

    pstmt->bindInt64(1, WallTime::now().secondsSinceEpoch().milliseconds());
    pstmt->bindInt(2, httpOnly ? *httpOnly : -1);
    pstmt->bindInt(3, secure ? *secure : -1);
    pstmt->bindInt(4, session ? *session : -1);
    pstmt->bindText(5, requestHost);

    if (CookieUtil::isIPAddress(requestHost) || !requestHost.contains('.') || registrableDomain.isEmpty())
        pstmt->bindNull(6);
    else
        pstmt->bindText(6, makeString("*."_s, registrableDomain.string()));

    Vector<Cookie> results;

    while (pstmt->step() == SQLITE_ROW) {

        String cookieName = pstmt->columnText(0);
        String cookieValue = pstmt->columnText(1);
        String cookieDomain = pstmt->columnText(2).convertToASCIILowercase();
        String cookiePath = pstmt->columnText(3);
        double cookieExpires = (double)pstmt->columnInt64(4);
        bool cookieHttpOnly = (pstmt->columnInt(5) == 1);
        bool cookieSecure = (pstmt->columnInt(6) == 1);
        bool cookieSession = (pstmt->columnInt(7) == 1);

        if (!CookieUtil::domainMatch(cookieDomain, requestHost))
            continue;

        // https://tools.ietf.org/html/rfc6265#section-5.1.4 "Paths and Path-Match"
        bool isPathMatched = cookiePath == requestPath
            || (requestPath.startsWith(cookiePath) && cookiePath.endsWith('/'))
            || (requestPath.startsWith(cookiePath) && (requestPath.codeUnitAt(cookiePath.length()) == '/'));

        if (!isPathMatched)
            continue;

        Cookie cookie;
        cookie.name = cookieName;
        cookie.value = cookieValue;
        cookie.domain = cookieDomain;
        cookie.path = cookiePath;
        if (cookieExpires)
            cookie.expires = cookieExpires;
        cookie.httpOnly = cookieHttpOnly;
        cookie.secure = cookieSecure;
        cookie.session = cookieSession;
        cookie.created = pstmt->columnDouble(8);
        auto sameSite = pstmt->columnInt(9);
        if (sameSite >= 0 && sameSite <= static_cast<int>(Cookie::SameSitePolicy::Strict))
            cookie.sameSite = static_cast<Cookie::SameSitePolicy>(sameSite);
        results.append(WTF::move(cookie));
    }

    return results;
}

Vector<Cookie> CookieJarDB::getAllCookies()
{
    Vector<Cookie> result;
    if (!isEnabled() || !m_database.isOpen())
        return result;

    auto pstmt = m_database.prepareStatement("SELECT name, value, domain, path, expires, httponly, secure, session, created, samesite FROM Cookie WHERE session = 1 OR expires > ? ORDER BY length(path) DESC, created, rowid"_s);
    if (!pstmt)
        return result;

    pstmt->bindInt64(1, WallTime::now().secondsSinceEpoch().milliseconds());
    while (pstmt->step() == SQLITE_ROW) {
        Cookie cookie;
        cookie.name = pstmt->columnText(0);
        cookie.value = pstmt->columnText(1);
        cookie.domain = pstmt->columnText(2).convertToASCIILowercase();
        cookie.path = pstmt->columnText(3);
        double cookieExpires = (double)pstmt->columnInt64(4);
        if (cookieExpires)
            cookie.expires = cookieExpires;
        cookie.httpOnly = (pstmt->columnInt(5) == 1);
        cookie.secure = (pstmt->columnInt(6) == 1);
        cookie.session = (pstmt->columnInt(7) == 1);
        cookie.created = pstmt->columnDouble(8);
        auto sameSite = pstmt->columnInt(9);
        if (sameSite >= 0 && sameSite <= static_cast<int>(Cookie::SameSitePolicy::Strict))
            cookie.sameSite = static_cast<Cookie::SameSitePolicy>(sameSite);
        result.append(WTF::move(cookie));
    }
    return result;
}

bool CookieJarDB::hasHttpOnlyCookie(const String& name, const String& domain, const String& path)
{
    auto& statement = preparedStatement(CHECK_EXISTS_HTTPONLY_COOKIE_SQL);

    statement.bindText(1, name);
    statement.bindText(2, domain);
    statement.bindText(3, path);

    return statement.step() == SQLITE_ROW;
}

static bool checkSecureCookie(const Cookie& cookie)
{
    if (cookie.name.startsWith("__Secure-"_s) && !cookie.secure)
        return false;

    // Cookies for __Host must have the Secure attribute, path explicitly set to "/", and no domain attribute
    if (cookie.name.startsWith("__Host-"_s) && (!cookie.secure || cookie.path != "/"_s || !cookie.domain.isEmpty()))
        return false;

    return true;
}

bool CookieJarDB::canAcceptCookie(const Cookie& cookie, const URL& firstParty, const URL& url, CookieJarDB::Source source)
{
    if (PublicSuffixStore::singleton().isPublicSuffix(cookie.domain))
        return false;

    bool fromJavaScript = source == CookieJarDB::Source::Script;
    if (fromJavaScript && (cookie.httpOnly || hasHttpOnlyCookie(cookie.name, cookie.domain, cookie.path)))
        return false;

    if (!CookieUtil::domainMatch(cookie.domain, url.host().convertToASCIILowercase()))
        return false;

    if (!checkCookieAcceptPolicy(firstParty, url))
        return false;

    return true;
}

static std::optional<Cookie> cookieFromRow(SQLiteStatement& statement)
{
    Cookie cookie;
    cookie.name = statement.columnText(0);
    cookie.value = statement.columnText(1);
    cookie.domain = statement.columnText(2);
    cookie.path = statement.columnText(3);
    cookie.session = statement.columnInt(7) == 1;
    if (!cookie.session)
        cookie.expires = static_cast<double>(statement.columnInt64(4));
    cookie.httpOnly = statement.columnInt(5) == 1;
    cookie.secure = statement.columnInt(6) == 1;
    cookie.created = statement.columnDouble(8);
    auto sameSite = statement.columnInt(9);
    if (sameSite < 0 || sameSite > static_cast<int>(Cookie::SameSitePolicy::Strict))
        return std::nullopt;
    cookie.sameSite = static_cast<Cookie::SameSitePolicy>(sameSite);
    return cookie;
}

static bool isExpired(const Cookie& cookie, double now)
{
    return !cookie.session && cookie.expires.value_or(0) <= now;
}

bool CookieJarDB::readCookie(const Cookie& key, std::optional<Cookie>& cookie)
{
    cookie = std::nullopt;
    auto statement = m_database.prepareStatement(SELECT_COOKIE_COLUMNS_SQL "WHERE name = ? AND domain = ? AND path = ?"_s);
    if (!statement || statement->bindText(1, key.name) != SQLITE_OK
        || statement->bindText(2, key.domain) != SQLITE_OK || statement->bindText(3, key.path) != SQLITE_OK)
        return false;
    auto code = statement->step();
    if (code == SQLITE_DONE)
        return true;
    if (code != SQLITE_ROW) {
        checkSQLiteReturnCode(code);
        return false;
    }
    cookie = cookieFromRow(*statement);
    if (!cookie || statement->step() != SQLITE_DONE) {
        cookie = std::nullopt;
        return false;
    }
    return true;
}

void CookieJarDB::setCookieChangeHandler(RefPtr<ChangeHandler>&& handler)
{
    m_cookieChangeHandler = WTF::move(handler);
    if (!m_cookieChangeHandler) {
        m_cookieExpirationTimer = nullptr;
        return;
    }
    if (!m_cookieExpirationTimer) {
        m_cookieExpirationTimer = makeUnique<RunLoop::Timer>(Ref { RunLoop::currentSingleton() }, "CookieJarDB::expiration"_s, [this] {
            cookieExpirationTimerFired();
        });
    }
    scheduleCookieExpiration();
}

void CookieJarDB::scheduleCookieExpiration()
{
    if (!m_cookieExpirationTimer)
        return;
    if (!isEnabled() || !m_database.isOpen()) {
        m_cookieExpirationTimer->stop();
        return;
    }
    auto statement = m_database.prepareStatement("SELECT MIN(expires), COUNT(*) FROM Cookie WHERE session = 0"_s);
    if (!statement || statement->step() != SQLITE_ROW) {
        m_cookieExpirationTimer->startOneShot(1_s);
        return;
    }
    if (!statement->columnInt64(1)) {
        m_cookieExpirationTimer->stop();
        return;
    }
    auto delay = Seconds::fromMilliseconds(statement->columnInt64(0) - WallTime::now().secondsSinceEpoch().milliseconds());
    // Recheck the wall clock at least once per minute while persistent cookies
    // exist. A clock adjustment must not postpone expiry for days.
    m_cookieExpirationTimer->startOneShot(std::clamp(delay, 1_ms, 60_s));
}

void CookieJarDB::cookieExpirationTimerFired()
{
    // A failed SQL deletion retries later. A successful sweep schedules the
    // next expiry before notifying observers, which may destroy this database.
    m_cookieExpirationTimer->startOneShot(1_s);
    deleteExpiredCookies();
}

void CookieJarDB::notifyCookieChanges(Vector<CookieChange>&& changes)
{
    // Keep the callback alive if it unregisters itself. All transactions and
    // statements have finished, and callers must not touch this database after
    // delivery: the callback may re-enter a mutation or destroy its owner.
    scheduleCookieExpiration();
    RefPtr handler = m_cookieChangeHandler;
    if (handler && !changes.isEmpty())
        handler->run(WTF::move(changes));
}

bool CookieJarDB::setCookie(const Cookie& cookie)
{
    std::optional<Cookie> storedCookie;
    return setCookieWithResult(cookie, storedCookie);
}

bool CookieJarDB::setCookieWithResult(const Cookie& cookie, std::optional<Cookie>& storedCookie)
{
    storedCookie = std::nullopt;
    if (!isEnabled() || !m_database.isOpen() || !cookie.partitionKey.isEmpty())
        return false;
    auto expires = cookie.expires.value_or(0.0);
    if (!std::isfinite(expires) || expires < static_cast<double>(std::numeric_limits<int64_t>::min())
        || expires >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return false;

    Vector<CookieChange> changes;
    std::optional<Cookie> result;
    {
        SQLiteTransaction transaction(m_database);
        transaction.begin();
        if (!transaction.inProgress())
            return false;
        std::optional<Cookie> previous;
        if (!readCookie(cookie, previous))
            return false;
        auto now = WallTime::now().secondsSinceEpoch().milliseconds();
        bool previousExpired = previous && isExpired(*previous, now);
        bool expiredReplacement = !cookie.session && expires <= now;
        if (previous && (previousExpired || expiredReplacement)) {
            auto statement = m_database.prepareStatement(DELETE_COOKIE_BY_NAME_DOMAIN_PATH_SQL);
            if (!statement || statement->bindText(1, cookie.name) != SQLITE_OK
                || statement->bindText(2, cookie.domain) != SQLITE_OK || statement->bindText(3, cookie.path) != SQLITE_OK
                || !checkSQLiteReturnCode(statement->step()) || m_database.lastChanges() != 1)
                return false;
        }
        if (!expiredReplacement) {
            auto statement = m_database.prepareStatement(SET_COOKIE_SQL);
            // FIXME: Add an eviction policy before reporting eviction events.
            if (!statement || statement->bindText(1, cookie.name) != SQLITE_OK
                || statement->bindText(2, cookie.value) != SQLITE_OK || statement->bindText(3, cookie.domain) != SQLITE_OK
                || statement->bindText(4, cookie.path) != SQLITE_OK
                || statement->bindInt64(5, cookie.session ? 0 : static_cast<int64_t>(expires)) != SQLITE_OK
                || statement->bindInt(6, cookie.value.length()) != SQLITE_OK
                || statement->bindInt(7, cookie.session ? 1 : 0) != SQLITE_OK
                || statement->bindInt(8, cookie.httpOnly ? 1 : 0) != SQLITE_OK
                || statement->bindInt(9, cookie.secure ? 1 : 0) != SQLITE_OK
                || statement->bindDouble(10, std::isfinite(cookie.created) && cookie.created > 0 ? cookie.created : now) != SQLITE_OK
                || statement->bindInt(11, static_cast<int>(cookie.sameSite)) != SQLITE_OK
                || !checkSQLiteReturnCode(statement->step()) || m_database.lastChanges() != 1)
                return false;
            if (!readCookie(cookie, result) || !result)
                return false;
        }
        if (previous)
            changes.append({ WTF::move(*previous), true, previousExpired ? CookieChangeCause::Expired : expiredReplacement ? CookieChangeCause::ExpiredOverwrite : CookieChangeCause::Overwrite });
        if (result)
            changes.append({ *result, false, CookieChangeCause::Explicit });
        transaction.commit();
        if (transaction.inProgress())
            return false;
    }
    // Capture the receipt before callbacks can perform another write.
    storedCookie = WTF::move(result);
    notifyCookieChanges(WTF::move(changes));
    return true;
}

bool CookieJarDB::setCookie(const URL& firstParty, const URL& url, const String& body, CookieJarDB::Source source, std::optional<Seconds> cappedLifetime)
{
    if (!isEnabled() || !m_database.isOpen())
        return false;

    if (url.isEmpty() || body.isEmpty())
        return false;

    auto cookie = CookieUtil::parseCookieHeader(body);
    if (!cookie || (cookie->name.isEmpty() && cookie->value.isEmpty()))
        return false;

    if (!checkSecureCookie(*cookie))
        return false;

    if (cookie->domain.isEmpty())
        cookie->domain = url.host().convertToASCIILowercase();

    if (cookie->path.isEmpty())
        cookie->path = CookieUtil::defaultPathForURL(url);

    if (!canAcceptCookie(*cookie, firstParty, url, source))
        return false;

    if (cappedLifetime && cookie->expires) {
        ASSERT(*cappedLifetime >= 0_s);
        auto cappedExpires = WallTime::now() + *cappedLifetime;
        if (cappedExpires < WallTime::fromRawSeconds(*cookie->expires / msPerSecond))
            cookie->expires = cappedExpires.secondsSinceEpoch().milliseconds();
    }

    return setCookie(*cookie);
}

HashSet<String> CookieJarDB::allDomains()
{
    auto statement = m_database.prepareStatement(SELECT_ALL_DOMAINS_SQL);
    if (!statement)
        return { };

    HashSet<String> domains;
    while (statement->step() == SQLITE_ROW) {
        auto domain = statement->columnText(0);
        domains.add(domain);
    }
    return domains;
}

bool CookieJarDB::deleteCookie(const Cookie& cookie)
{
    if (!cookie.partitionKey.isEmpty())
        return false;
    // An empty path is still an exact key for this API.
    return deleteCookiesMatching(SELECT_COOKIE_COLUMNS_SQL "WHERE name = ? AND domain = ? AND path = ? ORDER BY rowid"_s,
        DELETE_COOKIE_BY_NAME_DOMAIN_PATH_SQL, [&cookie](SQLiteStatement& statement) {
            return statement.bindText(1, cookie.name) == SQLITE_OK && statement.bindText(2, cookie.domain) == SQLITE_OK && statement.bindText(3, cookie.path) == SQLITE_OK;
        }, CookieChangeCause::Explicit);
}

bool CookieJarDB::deleteCookie(const String& url, const String& name)
{
    if (!isEnabled() || !m_database.isOpen())
        return false;

    String urlCopied = String(url);
    if (urlCopied.startsWith('.'))
        urlCopied = urlCopied.substring(1);

    URL urlObj({ }, urlCopied);
    if (urlObj.isValid()) {
        String hostStr(urlObj.host().toString());
        String pathStr(urlObj.path().toString());
        return deleteCookieInternal(name, hostStr, pathStr);
    }

    return false;
}

bool CookieJarDB::deleteCookieInternal(const String& name, const String& domain, const String& path)
{
    return deleteCookiesMatching(path.isEmpty() ? SELECT_COOKIE_COLUMNS_SQL "WHERE name = ? AND domain = ? ORDER BY rowid"_s
        : SELECT_COOKIE_COLUMNS_SQL "WHERE name = ? AND domain = ? AND path = ? ORDER BY rowid"_s,
        path.isEmpty() ? DELETE_COOKIE_BY_NAME_DOMAIN_SQL : DELETE_COOKIE_BY_NAME_DOMAIN_PATH_SQL,
        [&name, &domain, &path](SQLiteStatement& statement) {
            return statement.bindText(1, name) == SQLITE_OK && statement.bindText(2, domain) == SQLITE_OK
                && (path.isEmpty() || statement.bindText(3, path) == SQLITE_OK);
        }, CookieChangeCause::Explicit);
}

bool CookieJarDB::deleteCookiesMatching(ASCIILiteral selectSQL, ASCIILiteral deleteSQL, Function<bool(SQLiteStatement&)>&& bind, CookieChangeCause cause)
{
    if (!isEnabled() || !m_database.isOpen())
        return false;
    Vector<CookieChange> changes;
    {
        SQLiteTransaction transaction(m_database);
        transaction.begin();
        if (!transaction.inProgress())
            return false;
        auto now = WallTime::now().secondsSinceEpoch().milliseconds();
        {
            auto select = m_database.prepareStatement(selectSQL);
            if (!select || !bind(*select))
                return false;
            int code;
            while ((code = select->step()) == SQLITE_ROW) {
                auto cookie = cookieFromRow(*select);
                if (!cookie)
                    return false;
                auto removalCause = isExpired(*cookie, now) ? CookieChangeCause::Expired : cause;
                changes.append({ WTF::move(*cookie), true, removalCause });
            }
            if (code != SQLITE_DONE) {
                checkSQLiteReturnCode(code);
                return false;
            }
        }
        {
            auto statement = m_database.prepareStatement(deleteSQL);
            if (!statement || !bind(*statement) || !checkSQLiteReturnCode(statement->step())
                || static_cast<size_t>(m_database.lastChanges()) != changes.size())
                return false;
        }
        transaction.commit();
        if (transaction.inProgress())
            return false;
    }
    notifyCookieChanges(WTF::move(changes));
    return true;
}

bool CookieJarDB::deleteCookies(const String&)
{
    // NOT IMPLEMENTED
    // TODO: this function will be called if application calls WKCookieManagerDeleteCookiesForHostname() in WKCookieManager.h.
    return false;
}

bool CookieJarDB::deleteCookiesForHostname(const String& hostname, IncludeHttpOnlyCookies includeHttpOnlyCookies)
{
    return deleteCookiesMatching(includeHttpOnlyCookies == IncludeHttpOnlyCookies::Yes
        ? SELECT_COOKIE_COLUMNS_SQL "WHERE domain = ? ORDER BY rowid"_s
        : SELECT_COOKIE_COLUMNS_SQL "WHERE domain = ? AND httponly = 0 ORDER BY rowid"_s,
        includeHttpOnlyCookies == IncludeHttpOnlyCookies::Yes ? DELETE_COOKIES_BY_DOMAIN_SQL : DELETE_COOKIES_BY_DOMAIN_EXCEPT_HTTP_ONLY_SQL,
        [&hostname](SQLiteStatement& statement) { return statement.bindText(1, hostname) == SQLITE_OK; }, CookieChangeCause::Explicit);
}

bool CookieJarDB::deleteAllCookies()
{
    return deleteCookiesMatching(SELECT_COOKIE_COLUMNS_SQL "ORDER BY rowid"_s, DELETE_ALL_COOKIE_SQL,
        [](SQLiteStatement&) { return true; }, CookieChangeCause::Explicit);
}

bool CookieJarDB::deleteCookiesModifiedSince(WallTime since)
{
    // Clearing "all time" arrives as the epoch.
    if (since <= WallTime::fromRawSeconds(0))
        return deleteAllCookies();
    // created holds milliseconds; lastupdated is SQLite's CURRENT_TIMESTAMP text in UTC.
    auto milliseconds = since.secondsSinceEpoch().milliseconds();
    auto seconds = static_cast<int64_t>(since.secondsSinceEpoch().seconds());
    return deleteCookiesMatching(SELECT_COOKIE_COLUMNS_SQL "WHERE created >= ?1 OR lastupdated >= datetime(?2, 'unixepoch') ORDER BY rowid"_s,
        "DELETE FROM Cookie WHERE created >= ?1 OR lastupdated >= datetime(?2, 'unixepoch')"_s,
        [milliseconds, seconds](SQLiteStatement& statement) {
            return statement.bindDouble(1, milliseconds) == SQLITE_OK && statement.bindInt64(2, seconds) == SQLITE_OK;
        }, CookieChangeCause::Explicit);
}

bool CookieJarDB::deleteExpiredCookies()
{
    auto now = static_cast<int64_t>(WallTime::now().secondsSinceEpoch().milliseconds());
    return deleteCookiesMatching(SELECT_COOKIE_COLUMNS_SQL "WHERE session = 0 AND expires <= ? ORDER BY rowid"_s,
        "DELETE FROM Cookie WHERE session = 0 AND expires <= ?"_s,
        [now](SQLiteStatement& statement) { return statement.bindInt64(1, now) == SQLITE_OK; }, CookieChangeCause::Expired);
}

bool CookieJarDB::createPrepareStatement(ASCIILiteral sql)
{
    auto statement = m_database.prepareStatement(sql);
    if (!statement)
        return false;
    m_statements.add(sql, WTF::move(statement));
    return true;
}

SQLiteStatement& CookieJarDB::preparedStatement(const String& sql)
{
    const auto& statement = m_statements.get(sql);
    ASSERT(statement);
    statement->reset();
    return *statement;
}

bool CookieJarDB::executeSQLStatement(std::unique_ptr<SQLiteStatement>&& statement)
{
    if (!statement) {
        checkSQLiteReturnCode(m_database.lastError());
        LOG_ERROR("Failed to prepare sql statement with error: %s", m_database.lastErrorMsg());
        return false;
    }

    int ret = statement->step();
    if (!checkSQLiteReturnCode(ret)) {
        LOG_ERROR("Failed to execute SQL error: %s", m_database.lastErrorMsg());
        return false;
    }

    return true;
}

} // namespace WebCore
