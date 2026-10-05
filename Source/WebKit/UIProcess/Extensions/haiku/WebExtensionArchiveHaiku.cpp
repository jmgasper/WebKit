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
#include "WebExtensionArchiveHaiku.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionResourcePathsHaiku.h"
#include "WebExtensionCRXHaiku.h"
#include "WebExtensionStagingDirectoryHaiku.h"
#include <zip.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wtf/FileSystem.h>

namespace WebKit {

WebExtensionTemporaryDirectoryHaiku::WebExtensionTemporaryDirectoryHaiku(String&& path, String&& verifiedCRXIdentifier)
    : m_path(WTF::move(path)), m_verifiedCRXIdentifier(WTF::move(verifiedCRXIdentifier))
{
}

WebExtensionTemporaryDirectoryHaiku::WebExtensionTemporaryDirectoryHaiku(WebExtensionTemporaryDirectoryHaiku&& other)
    : m_path(other.release()), m_verifiedCRXIdentifier(std::exchange(other.m_verifiedCRXIdentifier, { }))
{
}

WebExtensionTemporaryDirectoryHaiku::~WebExtensionTemporaryDirectoryHaiku()
{
    if (!m_path.isEmpty())
        FileSystem::deleteNonEmptyDirectory(m_path);
}

String WebExtensionTemporaryDirectoryHaiku::release()
{
    return std::exchange(m_path, { });
}

namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) : m_descriptor(descriptor) { }
    FileDescriptor(FileDescriptor&& other) : m_descriptor(std::exchange(other.m_descriptor, -1)) { }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    ~FileDescriptor() { if (m_descriptor >= 0) close(m_descriptor); }
    int get() const { return m_descriptor; }
    int release() { return std::exchange(m_descriptor, -1); }
    explicit operator bool() const { return m_descriptor >= 0; }
    void reset(int descriptor)
    {
        if (m_descriptor >= 0)
            close(m_descriptor);
        m_descriptor = descriptor;
    }
private:
    int m_descriptor;
};

// ZIP names use forward slashes. Reject ambiguous native/Windows spellings,
// parent/current components, absolute paths, and names exceeding port bounds.
std::expected<std::vector<std::string>, WebExtensionArchiveErrorHaiku> pathComponents(const char* name, bool directory, const WebExtensionArchiveLimitsHaiku& limits)
{
    using Error = WebExtensionArchiveErrorHaiku;
    if (!name || !*name)
        return std::unexpected(Error::UnsafeEntry);
    std::string path(name);
    if (path.size() > limits.pathBytes)
        return std::unexpected(Error::ExpansionLimit);
    if (path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos
        || String::fromUTF8(path.c_str()).isNull())
        return std::unexpected(Error::UnsafeEntry);
    if (directory && path.back() == '/')
        path.pop_back();
    if (path.empty())
        return std::unexpected(Error::UnsafeEntry);

    std::vector<std::string> components;
    size_t begin = 0;
    while (begin <= path.size()) {
        auto end = path.find('/', begin);
        if (end == std::string::npos)
            end = path.size();
        auto component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." || component.size() > 255)
            return std::unexpected(Error::UnsafeEntry);
        components.push_back(WTF::move(component));
        if (components.size() > limits.pathDepth)
            return std::unexpected(Error::ExpansionLimit);
        begin = end + 1;
    }
    return components;
}

} // namespace

std::expected<WebExtensionTemporaryDirectoryHaiku, WebExtensionArchiveErrorHaiku> extractWebExtensionArchiveHaiku(const String& archivePath, const WebExtensionArchiveLimitsHaiku& limits, const String& temporaryParent)
{
    using Error = WebExtensionArchiveErrorHaiku;
    if (archivePath.isEmpty() || !archivePath.startsWith('/') || archivePath.contains('\0'))
        return std::unexpected(Error::InvalidArchive);
    auto inputBytes = archivePath.utf8();
    if (inputBytes.isNull())
        return std::unexpected(Error::InvalidArchive);
    FileDescriptor input(open(inputBytes.legacyCStringPointer(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat inputStatus { };
    if (!input || fstat(input.get(), &inputStatus) || !S_ISREG(inputStatus.st_mode) || inputStatus.st_size < 0)
        return std::unexpected(Error::InvalidArchive);
    if (static_cast<uint64_t>(inputStatus.st_size) > limits.archiveBytes)
        return std::unexpected(Error::ExpansionLimit);

    std::array<unsigned char, 4> signature;
    if (pread(input.get(), signature.data(), signature.size(), 0) != static_cast<ssize_t>(signature.size()))
        return std::unexpected(Error::InvalidArchive);
    // Keep verified bytes alive for libzip. It sees only the authenticated ZIP
    // payload, never CRX header bytes or a mutable source descriptor.
    std::vector<uint8_t> crxBytes;
    String verifiedCRXIdentifier;
    std::unique_ptr<zip_t, decltype(&zip_discard)> reader(nullptr, &zip_discard);
    if (signature == std::array<unsigned char, 4> { 'C', 'r', '2', '4' }) {
        crxBytes.resize(static_cast<size_t>(inputStatus.st_size));
        size_t offset = 0;
        while (offset < crxBytes.size()) {
            auto count = pread(input.get(), crxBytes.data() + offset, crxBytes.size() - offset, offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return std::unexpected(Error::InvalidArchive);
            offset += static_cast<size_t>(count);
        }
        struct stat after { };
        unsigned char extra;
        if (fstat(input.get(), &after) || after.st_size != inputStatus.st_size
            || after.st_mtime != inputStatus.st_mtime || after.st_ctime != inputStatus.st_ctime
            || pread(input.get(), &extra, 1, inputStatus.st_size) != 0)
            return std::unexpected(Error::InvalidArchive);
        auto verified = verifyWebExtensionCRXHaiku(crxBytes);
        if (!verified)
            return std::unexpected(verified.error() == WebExtensionCRXErrorHaiku::LimitExceeded ? Error::ExpansionLimit : Error::InvalidArchive);
        verifiedCRXIdentifier = String::fromUTF8(verified->identifier);
        zip_error_t error;
        zip_error_init(&error);
        auto* source = zip_source_buffer_create(crxBytes.data() + verified->archiveOffset, crxBytes.size() - verified->archiveOffset, 0, &error);
        if (source) {
            // Published Chromium archives can declare ZIP 4.5 locally and 2.0
            // centrally. libzip's extra consistency mode rejects that benign
            // mismatch. Read the verified immutable payload normally; resource
            // paths, types, sizes, encryption, CRC and cleanup remain checked.
            reader.reset(zip_open_from_source(source, ZIP_RDONLY, &error));
            if (!reader) zip_source_free(source);
        }
        zip_error_fini(&error);
    } else {
        // Ordinary ZIP/XPI containers cannot hide an executable/CRX prefix.
        if (signature[0] != 'P' || signature[1] != 'K'
            || !((signature[2] == 3 && signature[3] == 4) || (signature[2] == 5 && signature[3] == 6)))
            return std::unexpected(Error::InvalidArchive);
        int archiveError = 0;
        reader.reset(zip_fdopen(input.get(), ZIP_RDONLY | ZIP_CHECKCONS, &archiveError));
        if (reader) input.release(); // libzip owns the descriptor after success.
    }
    if (!reader)
        return std::unexpected(Error::InvalidArchive);
    auto entryCount = zip_get_num_entries(reader.get(), 0);
    if (entryCount < 0)
        return std::unexpected(Error::InvalidArchive);
    if (static_cast<uint64_t>(entryCount) > limits.entries)
        return std::unexpected(Error::ExpansionLimit);

    String parent = temporaryParent.isEmpty() ? webExtensionStagingDirectoryHaiku() : temporaryParent;
    if (parent.isEmpty()) {
        std::error_code fileSystemError;
        auto nativeParent = std::filesystem::temp_directory_path(fileSystemError).string();
        if (fileSystemError)
            return std::unexpected(Error::FileSystem);
        parent = String::fromUTF8(nativeParent.c_str());
    }
    auto canonicalParent = canonicalWebExtensionPathHaiku(parent);
    if (canonicalParent.isEmpty())
        return std::unexpected(Error::FileSystem);
    auto parentBytes = canonicalParent.utf8();
    std::string directoryTemplate(parentBytes.legacyCStringPointer());
    directoryTemplate += "/WebKitExtension-XXXXXX";
    if (!mkdtemp(directoryTemplate.data()))
        return std::unexpected(Error::FileSystem);
    WebExtensionTemporaryDirectoryHaiku directory(String::fromUTF8(directoryTemplate.c_str()), WTF::move(verifiedCRXIdentifier));
    FileDescriptor root(open(directoryTemplate.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!root)
        return std::unexpected(Error::FileSystem);

    unsigned createdNodes = 0;
    uint64_t expandedBytes = 0;
    bool hasManifest = false;
    std::unordered_set<std::string> seenEntries;
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index) {
        zip_stat_t entry;
        zip_stat_init(&entry);
        constexpr auto requiredFields = ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_COMP_METHOD | ZIP_STAT_ENCRYPTION_METHOD;
        if (zip_stat_index(reader.get(), index, ZIP_FL_ENC_STRICT, &entry) || (entry.valid & requiredFields) != requiredFields
            || entry.encryption_method != ZIP_EM_NONE
            || (entry.comp_method != ZIP_CM_STORE && entry.comp_method != ZIP_CM_DEFLATE))
            return std::unexpected(Error::InvalidArchive);
        zip_uint8_t operatingSystem;
        zip_uint32_t attributes;
        if (zip_file_get_external_attributes(reader.get(), index, 0, &operatingSystem, &attributes))
            return std::unexpected(Error::InvalidArchive);
        // ZIP's Unix/BeOS/macOS external mode bits retain the original type.
        // Never apply archive ownership, privileges, links, devices or ACLs.
        auto type = (attributes >> 16) & S_IFMT;
        if (type && type != S_IFREG && type != S_IFDIR)
            return std::unexpected(Error::UnsafeEntry);
        bool isDirectory = type == S_IFDIR || (!type && (attributes & 0x10));
        if (entry.name && *entry.name && entry.name[strlen(entry.name) - 1] == '/') {
            if (type == S_IFREG)
                return std::unexpected(Error::UnsafeEntry);
            isDirectory = true;
        }
        auto components = pathComponents(entry.name, isDirectory, limits);
        if (!components)
            return std::unexpected(components.error());
        std::string normalizedPath;
        for (const auto& component : *components) {
            if (!normalizedPath.empty())
                normalizedPath += '/';
            normalizedPath += component;
        }
        if (!seenEntries.insert(normalizedPath).second)
            return std::unexpected(Error::UnsafeEntry);
        auto fileBytes = entry.size;
        if (isDirectory && fileBytes)
            return std::unexpected(Error::InvalidArchive);
        if (fileBytes > limits.fileBytes || fileBytes > limits.expandedBytes - expandedBytes)
            return std::unexpected(Error::ExpansionLimit);

        FileDescriptor parentDirectory(fcntl(root.get(), F_DUPFD_CLOEXEC, 0));
        if (!parentDirectory)
            return std::unexpected(Error::FileSystem);
        auto directoryComponents = components->size() - (isDirectory ? 0 : 1);
        for (size_t index = 0; index < directoryComponents; ++index) {
            const auto& component = (*components)[index];
            if (!mkdirat(parentDirectory.get(), component.c_str(), 0700)) {
                if (++createdNodes > limits.entries)
                    return std::unexpected(Error::ExpansionLimit);
            } else if (errno != EEXIST)
                return std::unexpected(Error::FileSystem);
            auto child = openat(parentDirectory.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0)
                return std::unexpected(Error::UnsafeEntry);
            parentDirectory.reset(child);
        }
        if (isDirectory)
            continue;
        FileDescriptor output(openat(parentDirectory.get(), components->back().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!output)
            return std::unexpected(Error::UnsafeEntry);
        if (++createdNodes > limits.entries)
            return std::unexpected(Error::ExpansionLimit);

        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> archivedFile(zip_fopen_index(reader.get(), index, 0), &zip_fclose);
        if (!archivedFile)
            return std::unexpected(Error::InvalidArchive);
        uint64_t writtenBytes = 0;
        std::array<unsigned char, 64 * 1024> buffer;
        while (true) {
            auto count = zip_fread(archivedFile.get(), buffer.data(), buffer.size());
            if (count < 0)
                return std::unexpected(Error::InvalidArchive);
            if (!count)
                break;
            auto bytes = static_cast<uint64_t>(count);
            if (bytes > fileBytes - writtenBytes || bytes > limits.expandedBytes - expandedBytes)
                return std::unexpected(Error::ExpansionLimit);
            size_t offset = 0;
            while (offset < static_cast<size_t>(count)) {
                auto written = write(output.get(), buffer.data() + offset, static_cast<size_t>(count) - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    return std::unexpected(Error::FileSystem);
                offset += written;
            }
            writtenBytes += bytes;
            expandedBytes += bytes;
        }
        if (zip_fclose(archivedFile.release()) || writtenBytes != fileBytes)
            return std::unexpected(Error::InvalidArchive);
        if (close(output.release()))
            return std::unexpected(Error::FileSystem);
        if (normalizedPath == "manifest.json")
            hasManifest = true;
    }
    if (!hasManifest)
        return std::unexpected(Error::InvalidArchive);
    return directory;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
