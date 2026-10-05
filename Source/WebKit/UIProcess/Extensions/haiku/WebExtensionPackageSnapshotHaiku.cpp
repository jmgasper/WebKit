/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionPackageSnapshotHaiku.h"

#if PLATFORM(HAIKU)

#include "WebExtensionResourcePathsHaiku.h"
#include "WebExtensionStagingDirectoryHaiku.h"
#include "WebExtensionTimingHaiku.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <pal/crypto/CryptoDigest.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wtf/FileSystem.h>

namespace WebKit {

WebExtensionPackageSnapshotHaiku::WebExtensionPackageSnapshotHaiku(String&& path, String&& fingerprint, String&& verifiedCRXIdentifier, bool persistent)
    : m_path(WTF::move(path)), m_fingerprint(WTF::move(fingerprint)), m_verifiedCRXIdentifier(WTF::move(verifiedCRXIdentifier)), m_persistent(persistent) { }
WebExtensionPackageSnapshotHaiku::WebExtensionPackageSnapshotHaiku(WebExtensionPackageSnapshotHaiku&& other)
    : m_path(other.release()), m_fingerprint(WTF::move(other.m_fingerprint)), m_verifiedCRXIdentifier(WTF::move(other.m_verifiedCRXIdentifier)), m_persistent(other.m_persistent) { }
WebExtensionPackageSnapshotHaiku::~WebExtensionPackageSnapshotHaiku()
{
    if (!m_path.isEmpty() && !m_persistent)
        FileSystem::deleteNonEmptyDirectory(m_path);
}
String WebExtensionPackageSnapshotHaiku::release() { return std::exchange(m_path, { }); }

WebExtensionPackageSnapshotHaiku WebExtensionPackageSnapshotHaiku::isolatedCopy() &&
{
    return { release().isolatedCopy(), std::exchange(m_fingerprint, { }).isolatedCopy(), std::exchange(m_verifiedCRXIdentifier, { }).isolatedCopy(), m_persistent };
}

static std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> prepareWebExtensionArchiveFromCacheHaiku(const String& archive, const String& cacheDirectory, const WebExtensionArchiveLimitsHaiku&);

std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> prepareWebExtensionPackageHaiku(const String& path, const WebExtensionArchiveLimitsHaiku& limits, const String& temporaryParent)
{
    // An empty parent reaches extraction and snapshotting, which each fall back
    // to the configured staging directory.
    auto directory = canonicalWebExtensionPathHaiku(path);
    auto type = FileSystem::fileType(directory);
    if (directory.isEmpty() || (type != FileSystem::FileType::Directory && type != FileSystem::FileType::Regular))
        return std::unexpected(WebExtensionPackageErrorHaiku::InvalidPath);
    std::optional<WebExtensionTemporaryDirectoryHaiku> extraction;
    if (type == FileSystem::FileType::Regular) {
        // Unpacking a package creates every one of its files on BFS, which
        // costs milliseconds each; 1Password's thousand files took six
        // seconds at every launch. Archives are unpacked once into the
        // profile's package cache and verified by fingerprint afterwards.
        if (auto cache = webExtensionPackageCacheDirectoryHaiku(); !cache.isEmpty() && temporaryParent.isEmpty()) {
            auto cached = prepareWebExtensionArchiveFromCacheHaiku(directory, cache, limits);
            if (cached || cached.error() == WebExtensionPackageErrorHaiku::InvalidArchive)
                return cached;
            SUMMIT_EXTENSION_TIMING("package cache unusable for %s, unpacking into the staging directory", directory.utf8().legacyCStringPointer());
        }
        auto started = system_time();
        auto result = extractWebExtensionArchiveHaiku(directory, limits, temporaryParent);
        if (!result)
            return std::unexpected(WebExtensionPackageErrorHaiku::InvalidArchive);
        extraction.emplace(WTF::move(*result));
        directory = extraction->path();
        SUMMIT_EXTENSION_TIMING("unpacked %s in %.3f s", path.utf8().legacyCStringPointer(), (system_time() - started) / 1e6);
    }
    auto started = system_time();
    auto snapshot = snapshotWebExtensionPackageHaiku(directory, limits, temporaryParent);
    SUMMIT_EXTENSION_TIMING("snapshot of %s %s in %.3f s", path.utf8().legacyCStringPointer(), snapshot ? "copied" : "failed", (system_time() - started) / 1e6);
    if (!snapshot || !extraction)
        return snapshot;
    return WebExtensionPackageSnapshotHaiku(snapshot->release(), String(snapshot->fingerprint()), String(extraction->verifiedCRXIdentifier()));
}

namespace {
using Error = WebExtensionPackageErrorHaiku;
using Digest = PAL::Crypto::CryptoDigest;

class Descriptor {
public:
    explicit Descriptor(int value = -1) : m_value(value) { }
    Descriptor(const Descriptor&) = delete;
    ~Descriptor() { if (m_value >= 0) close(m_value); }
    int get() const { return m_value; }
    explicit operator bool() const { return m_value >= 0; }
    void reset(int value) { if (m_value >= 0) close(m_value); m_value = value; }
private:
    int m_value;
};

void addSize(Digest& digest, uint64_t size)
{
    std::array<uint8_t, 8> bytes;
    for (size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = size >> ((7 - index) * 8);
    digest.addBytes(bytes);
}

void addFrame(Digest& digest, uint8_t type, const std::string& path, uint64_t length)
{
    digest.addBytes(std::span { &type, 1 });
    addSize(digest, path.size());
    digest.addBytes(std::span { reinterpret_cast<const uint8_t*>(path.data()), path.size() });
    addSize(digest, length);
}

bool sameStatus(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

struct Walker {
    String root;
    const WebExtensionArchiveLimitsHaiku& limits;
    int rootDescriptor;
    std::unique_ptr<Digest> digest { Digest::create(Digest::Algorithm::SHA_256) };
    uint64_t bytes { 0 };
    unsigned entries { 0 };
    std::vector<std::pair<dev_t, ino_t>> ancestors;
    // Share a heap buffer across the traversal; recursive frames must fit on
    // the package worker's small native stack even at the path-depth limit.
    std::vector<uint8_t> buffer;

    Walker(const String& root, const WebExtensionArchiveLimitsHaiku& limits, int rootDescriptor) : root(root), limits(limits), rootDescriptor(rootDescriptor), buffer(64 * 1024)
    {
        auto domain = String("Kunanyi-WebExtension-Package-v1"_s).utf8();
        digest->addBytes(byteCast<uint8_t>(domain.span()));
    }

    int openContained(const String& path)
    {
        if (path == root)
            return openat(rootDescriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        auto bytes = path.utf8();
        auto rootBytes = root.utf8();
        std::string relative(bytes.legacyCStringPointer() + rootBytes.length() + (root.endsWith('/') ? 0 : 1));
        Descriptor parent(dup(rootDescriptor));
        if (!parent)
            return -1;
        size_t begin = 0;
        for (;;) {
            auto end = relative.find('/', begin);
            auto component = relative.substr(begin, end == std::string::npos ? end : end - begin);
            if (end == std::string::npos)
                return openat(parent.get(), component.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            parent.reset(openat(parent.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!parent)
                return -1;
            begin = end + 1;
        }
    }

    std::expected<void, Error> walk(int source, int destination, const std::string& physical, const std::string& logical, unsigned depth)
    {
        if (depth > limits.pathDepth || logical.size() > limits.pathBytes)
            return std::unexpected(Error::ExpansionLimit);
        struct stat before { };
        if (fstat(source, &before) || !S_ISDIR(before.st_mode))
            return std::unexpected(Error::FileSystem);
        auto identity = std::make_pair(before.st_dev, before.st_ino);
        if (std::find(ancestors.begin(), ancestors.end(), identity) != ancestors.end())
            return std::unexpected(Error::InvalidPath);
        ancestors.push_back(identity);
        addFrame(*digest, 'D', logical, 0);
        int enumerationDescriptor = dup(source);
        if (enumerationDescriptor < 0)
            return std::unexpected(Error::FileSystem);
        std::unique_ptr<DIR, decltype(&closedir)> directory(fdopendir(enumerationDescriptor), &closedir);
        if (!directory) {
            close(enumerationDescriptor);
            return std::unexpected(Error::FileSystem);
        }
        std::vector<std::string> names;
        errno = 0;
        while (auto* item = readdir(directory.get())) {
            std::string name(item->d_name);
            if (name == "." || name == "..")
                continue;
            if (String::fromUTF8(name).isNull())
                return std::unexpected(Error::InvalidPath);
            if (++entries > limits.entries)
                return std::unexpected(Error::ExpansionLimit);
            names.push_back(WTF::move(name));
            errno = 0;
        }
        if (errno)
            return std::unexpected(Error::FileSystem);
        std::sort(names.begin(), names.end());
        for (auto& name : names) {
            auto relative = logical.empty() ? name : logical + "/" + name;
            if (depth + 1 > limits.pathDepth || relative.size() > limits.pathBytes)
                return std::unexpected(Error::ExpansionLimit);
            struct stat entryBefore { };
            if (fstatat(source, name.c_str(), &entryBefore, AT_SYMLINK_NOFOLLOW))
                return std::unexpected(Error::SourceChanged);
            auto sourcePath = physical + "/" + name;
            auto canonical = canonicalWebExtensionPathHaiku(String::fromUTF8(sourcePath));
            if (canonical.isEmpty() || (canonical != root && !FileSystem::isAncestor(root, canonical)))
                return std::unexpected(Error::InvalidPath);
            auto resolvedBytes = canonical.utf8();
            bool symbolic = S_ISLNK(entryBefore.st_mode);
            Descriptor input(symbolic ? openContained(canonical)
                                      : openat(source, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            struct stat opened { };
            if (!input || fstat(input.get(), &opened))
                return std::unexpected(Error::FileSystem);
            if (!symbolic && !sameStatus(entryBefore, opened))
                return std::unexpected(Error::SourceChanged);
            if (S_ISDIR(opened.st_mode)) {
                if (destination >= 0 && mkdirat(destination, name.c_str(), 0700))
                    return std::unexpected(Error::FileSystem);
                Descriptor output(destination >= 0 ? openat(destination, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1);
                if (destination >= 0 && !output)
                    return std::unexpected(Error::FileSystem);
                auto result = walk(input.get(), output.get(), resolvedBytes.legacyCStringPointer(), relative, depth + 1);
                if (!result)
                    return result;
            } else if (S_ISREG(opened.st_mode)) {
                if (opened.st_size < 0 || static_cast<uint64_t>(opened.st_size) > limits.fileBytes
                    || static_cast<uint64_t>(opened.st_size) > limits.expandedBytes - bytes)
                    return std::unexpected(Error::ExpansionLimit);
                bytes += opened.st_size;
                Descriptor output(destination >= 0 ? openat(destination, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600) : -1);
                if (destination >= 0 && !output)
                    return std::unexpected(Error::FileSystem);
                addFrame(*digest, 'F', relative, opened.st_size);
                uint64_t copied = 0;
                for (;;) {
                    auto count = read(input.get(), buffer.data(), buffer.size());
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0)
                        return std::unexpected(Error::FileSystem);
                    if (!count)
                        break;
                    if (static_cast<uint64_t>(count) > static_cast<uint64_t>(opened.st_size) - copied)
                        return std::unexpected(Error::SourceChanged);
                    copied += count;
                    auto span = std::span { buffer }.first(count);
                    digest->addBytes(span);
                    if (destination >= 0) {
                        size_t written = 0;
                        while (written < span.size()) {
                            auto count = write(output.get(), span.data() + written, span.size() - written);
                            if (count < 0 && errno == EINTR)
                                continue;
                            if (count <= 0)
                                return std::unexpected(Error::FileSystem);
                            written += count;
                        }
                    }
                }
                if (copied != static_cast<uint64_t>(opened.st_size))
                    return std::unexpected(Error::SourceChanged);
                if (destination >= 0 && fsync(output.get()))
                    return std::unexpected(Error::FileSystem);
            } else
                return std::unexpected(Error::InvalidPath);
            struct stat after { }, entryAfter { };
            if (fstat(input.get(), &after) || fstatat(source, name.c_str(), &entryAfter, AT_SYMLINK_NOFOLLOW)
                || !sameStatus(opened, after) || !sameStatus(entryBefore, entryAfter)
                || canonicalWebExtensionPathHaiku(String::fromUTF8(sourcePath)) != canonical)
                return std::unexpected(Error::SourceChanged);
        }
        struct stat after { };
        if (fstat(source, &after) || !sameStatus(before, after))
            return std::unexpected(Error::SourceChanged);
        ancestors.pop_back();
        return { };
    }
};
} // namespace

std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> snapshotWebExtensionPackageHaiku(const String& directory, const WebExtensionArchiveLimitsHaiku& limits, const String& requestedTemporaryParent)
{
    auto temporaryParent = requestedTemporaryParent.isEmpty() ? webExtensionStagingDirectoryHaiku() : requestedTemporaryParent;
    auto root = canonicalWebExtensionPathHaiku(directory);
    if (root.isEmpty())
        return std::unexpected(Error::InvalidPath);
    auto rootBytes = root.utf8();
    Descriptor source(open(rootBytes.legacyCStringPointer(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat before { };
    if (!source || fstat(source.get(), &before))
        return std::unexpected(Error::InvalidPath);
    String parent = temporaryParent;
    if (parent.isEmpty()) {
        std::error_code error;
        auto temporary = std::filesystem::temp_directory_path(error);
        if (error)
            return std::unexpected(Error::FileSystem);
        parent = String::fromUTF8(temporary.string());
    }
    auto canonicalParent = canonicalWebExtensionPathHaiku(parent);
    if (canonicalParent.isEmpty() || canonicalParent == root || FileSystem::isAncestor(root, canonicalParent))
        return std::unexpected(Error::InvalidPath);
    std::string path = canonicalParent.utf8().legacyCStringPointer();
    path += "/WebKitPackage-XXXXXX";
    if (!mkdtemp(path.data()))
        return std::unexpected(Error::FileSystem);
    WebExtensionPackageSnapshotHaiku owner(String::fromUTF8(path), { });
    Descriptor destination(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!destination)
        return std::unexpected(Error::FileSystem);
    Walker copy(root, limits, source.get());
    auto copied = copy.walk(source.get(), destination.get(), rootBytes.legacyCStringPointer(), { }, 0);
    if (!copied)
        return std::unexpected(copied.error());
    auto fingerprint = copy.digest->toHexString();
    // Reopen, enumerate and hash the source again. This catches changes to an
    // earlier file after it was copied, as well as additions/removals and aliases.
    Descriptor verifySource(open(rootBytes.legacyCStringPointer(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat current { };
    if (!verifySource || fstat(verifySource.get(), &current) || !sameStatus(before, current))
        return std::unexpected(Error::SourceChanged);
    Walker verify(root, limits, verifySource.get());
    auto verified = verify.walk(verifySource.get(), -1, rootBytes.legacyCStringPointer(), { }, 0);
    if (!verified)
        return std::unexpected(verified.error());
    if (verify.digest->toHexString() != fingerprint)
        return std::unexpected(Error::SourceChanged);
    return WebExtensionPackageSnapshotHaiku(owner.release(), WTF::move(fingerprint));
}

namespace {

// A cache entry is <cache>/<key>/ holding "package" (the unpacked archive)
// and "record" (the archive path, size and modification time it came from,
// the resource fingerprint and the verified CRX signer). The key is the
// SHA-256 of the archive's bytes, so a replaced archive gets a new entry
// and the record's archive path lets the old one be removed.
struct CacheRecord {
    std::string archive;
    uint64_t size { 0 };
    int64_t modified { 0 };
    String fingerprint;
    String verifiedCRXIdentifier;
};

bool readCacheRecord(const std::string& path, CacheRecord& record)
{
    Descriptor input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!input)
        return false;
    std::string text;
    char buffer[4096];
    for (;;) {
        auto count = read(input.get(), buffer, sizeof buffer);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        text.append(buffer, count);
        if (text.size() > 64 * 1024)
            return false;
    }
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find('\n', begin);
        if (end == std::string::npos)
            return false;
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    if (lines.size() != 6 || lines[0] != "Kunanyi-WebExtension-PackageCache-v1")
        return false;
    record.archive = lines[1];
    record.size = strtoull(lines[2].c_str(), nullptr, 10);
    record.modified = strtoll(lines[3].c_str(), nullptr, 10);
    record.fingerprint = String::fromUTF8(lines[4].c_str());
    record.verifiedCRXIdentifier = String::fromUTF8(lines[5].c_str());
    return record.fingerprint.length() == 64 && !record.archive.empty();
}

bool writeCacheRecord(const std::string& path, const CacheRecord& record)
{
    std::string text = "Kunanyi-WebExtension-PackageCache-v1\n";
    text += record.archive + "\n";
    text += std::to_string(record.size) + "\n";
    text += std::to_string(record.modified) + "\n";
    text += record.fingerprint.utf8().legacyCStringPointer();
    text += "\n";
    text += record.verifiedCRXIdentifier.utf8().legacyCStringPointer();
    text += "\n";
    Descriptor output(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!output)
        return false;
    size_t written = 0;
    while (written < text.size()) {
        auto count = write(output.get(), text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        written += count;
    }
    return !fsync(output.get());
}

std::optional<std::string> archiveDigest(const std::string& archive, uint64_t& size, int64_t& modified)
{
    Descriptor input(open(archive.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat status { };
    if (!input || fstat(input.get(), &status) || !S_ISREG(status.st_mode))
        return std::nullopt;
    auto digest = Digest::create(Digest::Algorithm::SHA_256);
    std::vector<uint8_t> buffer(256 * 1024);
    for (;;) {
        auto count = read(input.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return std::nullopt;
        if (!count)
            break;
        digest->addBytes(std::span { buffer }.first(count));
    }
    size = status.st_size;
    modified = static_cast<int64_t>(status.st_mtim.tv_sec) * 1000000000 + status.st_mtim.tv_nsec;
    return digest->toHexString().utf8().legacyCStringPointer();
}

// Hash the unpacked tree without copying it: the same digest the copying walk
// computes, so it can be compared with the recorded and approved fingerprint.
std::optional<String> fingerprintOfDirectory(const String& directory, const WebExtensionArchiveLimitsHaiku& limits)
{
    auto root = canonicalWebExtensionPathHaiku(directory);
    if (root.isEmpty())
        return std::nullopt;
    auto rootBytes = root.utf8();
    Descriptor source(open(rootBytes.legacyCStringPointer(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!source)
        return std::nullopt;
    Walker walker(root, limits, source.get());
    if (!walker.walk(source.get(), -1, rootBytes.legacyCStringPointer(), { }, 0))
        return std::nullopt;
    return walker.digest->toHexString();
}

// Drop entries made from an archive that no longer exists, entries for this
// archive's path with another key (the package was replaced), and unpacking
// directories abandoned by an interrupted launch.
void pruneCache(const String& cacheDirectory, const std::string& archive, const std::string& currentKey)
{
    for (auto& name : FileSystem::listDirectory(cacheDirectory)) {
        auto path = FileSystem::pathByAppendingComponent(cacheDirectory, name);
        auto bytes = name.utf8();
        std::string entry(bytes.legacyCStringPointer(), bytes.length());
        if (entry == currentKey)
            continue;
        bool remove = false;
        if (entry.find(".tmp-") != std::string::npos) {
            auto modified = FileSystem::fileModificationTime(path);
            remove = !modified || WallTime::now() - *modified > 3600_s;
        } else {
            CacheRecord record;
            if (!readCacheRecord(FileSystem::pathByAppendingComponent(path, "record"_s).utf8().legacyCStringPointer(), record))
                remove = true;
            else {
                struct stat status { };
                remove = record.archive == archive || (stat(record.archive.c_str(), &status) && errno == ENOENT);
            }
        }
        if (remove) {
            FileSystem::deleteNonEmptyDirectory(path);
            SUMMIT_EXTENSION_TIMING("removed package cache entry %s", entry.c_str());
        }
    }
}

} // namespace

static std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> prepareWebExtensionArchiveFromCacheHaiku(const String& archive, const String& cacheDirectory, const WebExtensionArchiveLimitsHaiku& limits)
{
    using Error = WebExtensionPackageErrorHaiku;
    auto started = system_time();
    std::string archiveBytes = archive.utf8().legacyCStringPointer();
    uint64_t size = 0;
    int64_t modified = 0;
    auto key = archiveDigest(archiveBytes, size, modified);
    if (!key)
        return std::unexpected(Error::InvalidArchive);
    if (!FileSystem::makeAllDirectories(cacheDirectory))
        return std::unexpected(Error::FileSystem);
    auto entry = FileSystem::pathByAppendingComponent(cacheDirectory, String::fromUTF8(key->c_str()));
    auto package = FileSystem::pathByAppendingComponent(entry, "package"_s);
    std::string recordPath = FileSystem::pathByAppendingComponent(entry, "record"_s).utf8().legacyCStringPointer();
    CacheRecord record;
    if (readCacheRecord(recordPath, record) && FileSystem::fileType(package) == FileSystem::FileType::Directory) {
        auto fingerprint = fingerprintOfDirectory(package, limits);
        if (fingerprint && *fingerprint == record.fingerprint) {
            SUMMIT_EXTENSION_TIMING("package cache hit for %s, verified in %.3f s", archiveBytes.c_str(), (system_time() - started) / 1e6);
            pruneCache(cacheDirectory, archiveBytes, *key);
            return WebExtensionPackageSnapshotHaiku(canonicalWebExtensionPathHaiku(package), String(record.fingerprint), String(record.verifiedCRXIdentifier), true);
        }
        SUMMIT_EXTENSION_TIMING("package cache entry for %s does not match its record; unpacking again", archiveBytes.c_str());
        FileSystem::deleteNonEmptyDirectory(entry);
    }
    std::string temporary = FileSystem::pathByAppendingComponent(cacheDirectory, String::fromUTF8((*key + ".tmp-XXXXXX").c_str())).utf8().legacyCStringPointer();
    if (!mkdtemp(temporary.data()))
        return std::unexpected(Error::FileSystem);
    // The unpacking directory is owned until the entry is complete, so a
    // failure on the way leaves nothing behind.
    WebExtensionPackageSnapshotHaiku owner(String::fromUTF8(temporary.c_str()), { });
    auto extraction = extractWebExtensionArchiveHaiku(archive, limits, String::fromUTF8(temporary.c_str()));
    if (!extraction)
        return std::unexpected(Error::InvalidArchive);
    auto unpacked = system_time();
    auto fingerprint = fingerprintOfDirectory(extraction->path(), limits);
    if (!fingerprint)
        return std::unexpected(Error::FileSystem);
    std::string packageInTemporary = temporary + "/package";
    if (rename(extraction->path().utf8().legacyCStringPointer(), packageInTemporary.c_str()))
        return std::unexpected(Error::FileSystem);
    extraction->release();
    record = { archiveBytes, size, modified, *fingerprint, String(extraction->verifiedCRXIdentifier()) };
    if (!writeCacheRecord(temporary + "/record", record))
        return std::unexpected(Error::FileSystem);
    std::string entryBytes = entry.utf8().legacyCStringPointer();
    if (rename(temporary.c_str(), entryBytes.c_str())) {
        // Another preparation of the same archive finished first; use its entry.
        if (errno != EEXIST && errno != ENOTEMPTY)
            return std::unexpected(Error::FileSystem);
        CacheRecord existing;
        if (!readCacheRecord(recordPath, existing) || existing.fingerprint != *fingerprint)
            return std::unexpected(Error::FileSystem);
    }
    owner.release();
    SUMMIT_EXTENSION_TIMING("package cache miss for %s: unpacked in %.3f s, hashed in %.3f s", archiveBytes.c_str(), (unpacked - started) / 1e6, (system_time() - unpacked) / 1e6);
    pruneCache(cacheDirectory, archiveBytes, *key);
    return WebExtensionPackageSnapshotHaiku(canonicalWebExtensionPathHaiku(package), WTF::move(*fingerprint), String(record.verifiedCRXIdentifier), true);
}

String fingerprintWebExtensionMemoryResourcesHaiku(const String& manifestJSON, Vector<WebExtensionMemoryResourceHaiku>&& data, Vector<WebExtensionMemoryResourceHaiku>&& strings)
{
    auto digest = Digest::create(Digest::Algorithm::SHA_256);
    auto domain = String("Kunanyi-WebExtension-Memory-v1"_s).utf8();
    digest->addBytes(byteCast<uint8_t>(domain.span()));
    auto manifest = manifestJSON.utf8();
    addFrame(*digest, 'M', { }, manifest.length());
    digest->addBytes(byteCast<uint8_t>(manifest.span()));
    auto add = [&](auto& resources, uint8_t type) {
        struct Entry { std::string path; std::span<const uint8_t> bytes; };
        std::vector<Entry> entries;
        for (auto& resource : resources) {
            auto bytes = resource.path.utf8();
            entries.push_back({ std::string(bytes.legacyCStringPointer(), bytes.length()), resource.bytes });
        }
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.path < b.path; });
        for (auto& entry : entries) {
            addFrame(*digest, type, entry.path, entry.bytes.size());
            digest->addBytes(entry.bytes);
        }
    };
    add(data, 'B');
    add(strings, 'S');
    return digest->toHexString();
}

} // namespace WebKit

#endif // PLATFORM(HAIKU)
