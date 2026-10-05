/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "CachedScriptSourceProvider.h"

#if PLATFORM(HAIKU)

#include "CommonVM.h"
#include <JavaScriptCore/BytecodeCacheError.h>
#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/CachedTypes.h>
#include <JavaScriptCore/JSCBytecodeCacheVersion.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/UnlinkedFunctionCodeBlock.h>
#include <JavaScriptCore/UnlinkedFunctionExecutable.h>
#include <limits>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <wtf/Deque.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashSet.h>
#include <wtf/MallocSpan.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>

// A site's scripts are compiled again on every visit: parsed in full once to
// check them, then each function parsed again and turned into bytecode the
// first time it runs. YouTube's 2.5 MB of script cost its web process half a
// second of that on every load. JavaScriptCore can keep that work on disk
// (CachedBytecode): the program's bytecode when it is compiled, and each
// function's as it is compiled later, decoded lazily on the next visit. The
// jsc shell uses it through its own SourceProvider; here the scripts a page
// loads do the same.
//
// The directory comes from the browser (WEBKIT_HAIKU_BYTECODE_CACHE, set for
// the persistent profile by WebViewContextHaiku); private sessions keep
// nothing. SUMMIT_BYTECODE_CACHE=0 turns it off and
// SUMMIT_BYTECODE_CACHE_TRACE=1 reports each load and write.

namespace WebCore {

// Functions compiled since the program was cached or read, kept until the
// file is written: encoding each one as it is compiled cost YouTube's first
// load 130-290 ms before DOMContentLoaded.
struct CachedScriptSourceProvider::PendingFunctionsHaiku {
    struct Function {
        JSC::Strong<JSC::UnlinkedFunctionExecutable> executable;
        JSC::CodeSpecializationKind kind;
        JSC::Strong<JSC::UnlinkedFunctionCodeBlock> codeBlock;
    };
    Vector<Function> functions;
    size_t next { 0 };
};

// Small scripts are cheap to compile; a file each would cost more to open.
static constexpr unsigned minimumSourceLengthHaiku = 20 * 1024;
// The program and the functions compiled in its first seconds are written
// once, this long after the script first ran.
static constexpr Seconds commitDelayHaiku = 5_s;
static constexpr uint64_t directoryLimitHaiku = 320 * MB;
static constexpr uint64_t directoryTargetHaiku = 240 * MB;

static bool traceHaiku()
{
    static bool trace = [] {
        const char* value = getenv("SUMMIT_BYTECODE_CACHE_TRACE");
        return value && *value && strcmp(value, "0");
    }();
    return trace;
}

// Oldest files first, until the directory is under its target; other
// builds' directories go whole. Once per process, well after it starts.
static void pruneDirectoriesHaiku(const String& base, const String& current)
{
    Thread::create("Bytecode cache pruning"_s, [base = base.isolatedCopy(), current = current.isolatedCopy()] {
        sleep(30_s);
        for (auto& name : FileSystem::listDirectory(base)) {
            auto path = FileSystem::pathByAppendingComponent(base, name);
            if (path != current && FileSystem::fileType(path) == FileSystem::FileType::Directory)
                FileSystem::deleteNonEmptyDirectory(path);
        }
        struct Entry {
            CString path;
            uint64_t size;
            time_t time;
        };
        Vector<Entry> entries;
        uint64_t total = 0;
        time_t now = time(nullptr);
        for (auto& name : FileSystem::listDirectory(current)) {
            auto path = FileSystem::pathByAppendingComponent(current, name).utf8();
            struct stat info;
            if (stat(path.legacyCStringPointer(), &info) || !S_ISREG(info.st_mode))
                continue;
            // A write that never finished.
            if (name.endsWith(".tmp"_s)) {
                if (now - info.st_mtime > 60)
                    unlink(path.legacyCStringPointer());
                continue;
            }
            entries.append({ WTF::move(path), static_cast<uint64_t>(info.st_size), info.st_mtime });
            total += info.st_size;
        }
        if (total <= directoryLimitHaiku)
            return;
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.time < b.time; });
        for (auto& entry : entries) {
            if (total <= directoryTargetHaiku)
                break;
            if (!unlink(entry.path.legacyCStringPointer()))
                total -= entry.size;
        }
    }, ThreadType::Unknown, ThreadQOS::Background)->detach();
}

// Cached bytecode can only be read by the JavaScriptCore that wrote it. Its
// own check (computeJSCBytecodeCacheVersion()) uses the library's build ID,
// which these libraries do not have, so the library's size and time name
// the directory as well.
static const String& directoryHaiku()
{
    static NeverDestroyed<String> directory = [] -> String {
        const char* base = getenv("WEBKIT_HAIKU_BYTECODE_CACHE");
        if (!base || *base != '/')
            return { };
        if (const char* setting = getenv("SUMMIT_BYTECODE_CACHE"); setting && !strcmp(setting, "0"))
            return { };
        Dl_info info { };
        struct stat library;
        if (!dladdr(reinterpret_cast<void*>(&JSC::computeJSCBytecodeCacheVersion), &info) || !info.dli_fname || stat(info.dli_fname, &library))
            return { };
        uint64_t key = (static_cast<uint64_t>(JSC::computeJSCBytecodeCacheVersion()) << 32)
            ^ static_cast<uint64_t>(library.st_size) ^ (static_cast<uint64_t>(library.st_mtime) << 24);
        auto baseDirectory = String::fromUTF8(base);
        auto directory = FileSystem::pathByAppendingComponent(baseDirectory, makeString(hex(key, 16)));
        if (!FileSystem::makeAllDirectories(directory))
            return { };
        pruneDirectoriesHaiku(baseDirectory, directory);
        return directory;
    }();
    return directory.get();
}

String CachedScriptSourceProvider::bytecodeCachePathHaiku() const
{
    auto& directory = directoryHaiku();
    if (directory.isNull())
        return { };
    // JavaScriptCore encodes programs and modules only.
    if (sourceType() != JSC::SourceProviderSourceType::Program && sourceType() != JSC::SourceProviderSourceType::Module)
        return { };
    if (m_cachedScript->sessionID().isEphemeral())
        return { };
    unsigned length = source().length();
    if (length < minimumSourceLengthHaiku)
        return { };
    return FileSystem::pathByAppendingComponent(directory, makeString(hex(hash(), 8), '-', hex(length, 8), isModuleType() ? ".module"_s : ".script"_s));
}

RefPtr<JSC::CachedBytecode> CachedScriptSourceProvider::cachedBytecode() const
{
    if (m_triedBytecodeLoadHaiku)
        return m_cachedBytecodeHaiku;
    m_triedBytecodeLoadHaiku = true;
    auto path = bytecodeCachePathHaiku();
    if (path.isNull())
        return nullptr;
    auto handle = FileSystem::openFile(path, FileSystem::FileOpenMode::Read);
    if (!handle)
        return nullptr;
    auto data = handle.map(FileSystem::MappedFileMode::Private);
    if (!data || !data->size())
        return nullptr;
    m_cachedBytecodeFileSizeHaiku = data->size();
    m_cachedBytecodeHaiku = JSC::CachedBytecode::create(WTF::move(*data));
    // Recently used, for pruning.
    utime(path.utf8().legacyCStringPointer(), nullptr);
    if (traceHaiku())
        fprintf(stderr, "bytecode cache: read %zu bytes for %s\n", m_cachedBytecodeFileSizeHaiku, sourceURL().utf8().legacyCStringPointer());
    scheduleBytecodeCommitHaiku();
    return m_cachedBytecodeHaiku;
}

void CachedScriptSourceProvider::cacheBytecode(const JSC::BytecodeCacheGenerator& generator) const
{
    // The program was compiled, so whatever was read did not fit (or there
    // was nothing): the file is written anew.
    m_triedBytecodeLoadHaiku = true;
    m_cachedBytecodeHaiku = nullptr;
    m_cachedBytecodeFileSizeHaiku = 0;
    if (bytecodeCachePathHaiku().isNull())
        return;
    auto start = MonotonicTime::now();
    auto update = generator();
    double milliseconds = (MonotonicTime::now() - start).milliseconds();
    m_encodeMillisecondsHaiku += milliseconds;
    if (traceHaiku())
        fprintf(stderr, "bytecode cache: program encoded in %.1f ms for %s\n", milliseconds, sourceURL().utf8().legacyCStringPointer());
    if (!update)
        return;
    m_cachedBytecodeHaiku = JSC::CachedBytecode::create();
    m_cachedBytecodeHaiku->addGlobalUpdate(*update);
    scheduleBytecodeCommitHaiku();
}

void CachedScriptSourceProvider::updateCache(const JSC::UnlinkedFunctionExecutable* executable, const JSC::SourceCode&, JSC::CodeSpecializationKind kind, const JSC::UnlinkedFunctionCodeBlock* codeBlock) const
{
    if (!m_cachedBytecodeHaiku)
        return;
    if (!m_pendingFunctionsHaiku)
        m_pendingFunctionsHaiku = new PendingFunctionsHaiku;
    auto& vm = commonVM();
    m_pendingFunctionsHaiku->functions.append({ JSC::Strong<JSC::UnlinkedFunctionExecutable>(vm, const_cast<JSC::UnlinkedFunctionExecutable*>(executable)), kind,
        JSC::Strong<JSC::UnlinkedFunctionCodeBlock>(vm, const_cast<JSC::UnlinkedFunctionCodeBlock*>(codeBlock)) });
}

// Returns whether every recorded function has been encoded.
bool CachedScriptSourceProvider::encodePendingFunctionsHaiku(double budgetMilliseconds) const
{
    auto* pending = m_pendingFunctionsHaiku;
    if (!pending || !m_cachedBytecodeHaiku)
        return true;
    auto start = MonotonicTime::now();
    auto& vm = commonVM();
    while (pending->next < pending->functions.size()) {
        auto& function = pending->functions[pending->next++];
        auto* executable = function.executable.get();
        // Recorded in the order they were compiled, so an enclosing function
        // is added (and its own functions with it) before those inside it;
        // addFunctionUpdate() needs the function's place in the file.
        if (executable && m_cachedBytecodeHaiku->leafExecutables().contains(executable)) {
            JSC::BytecodeCacheError error;
            auto update = JSC::encodeFunctionCodeBlock(vm, function.codeBlock.get(), error);
            if (update && !error.isValid())
                m_cachedBytecodeHaiku->addFunctionUpdate(executable, function.kind, *update);
        }
        function.executable.clear();
        function.codeBlock.clear();
        if ((MonotonicTime::now() - start).milliseconds() >= budgetMilliseconds)
            break;
    }
    m_encodeMillisecondsHaiku += (MonotonicTime::now() - start).milliseconds();
    return pending->next >= pending->functions.size();
}

void CachedScriptSourceProvider::discardPendingBytecodeHaiku()
{
    delete std::exchange(m_pendingFunctionsHaiku, nullptr);
}

static bool writeAllHaiku(int fd, std::span<const uint8_t> data)
{
    while (!data.empty()) {
        ssize_t written = write(fd, data.data(), data.size());
        if (written <= 0)
            return false;
        data = data.subspan(written);
    }
    return true;
}

// Writes go to a thread of their own: the file is synced before it replaces
// the old one, since BFS journals only metadata and after a power cut a
// renamed file could hold stale blocks, which JavaScriptCore would decode.
static WorkQueue& writerQueueHaiku()
{
    static NeverDestroyed<Ref<WorkQueue>> queue = WorkQueue::create("Bytecode cache writer"_s, WorkQueue::QOS::Utility);
    return queue.get();
}

// Main thread: providers whose functions are being encoded a few
// milliseconds at a time, one after the other.
static HashSet<const void*>& committingProvidersHaiku()
{
    static NeverDestroyed<HashSet<const void*>> providers;
    return providers.get();
}
static Deque<Ref<const CachedScriptSourceProvider>>& commitQueueHaiku()
{
    static NeverDestroyed<Deque<Ref<const CachedScriptSourceProvider>>> queue;
    return queue.get();
}
static bool s_commitStepScheduledHaiku { false };

// Encoding is work for when the page has nothing better to do. Run back to
// back, the 4 ms slices took YouTube's main thread for 580-700 ms of the
// seconds in which its home page was still filling in (issue #22). The next
// slice waits at least 12 ms, so encoding takes a quarter of the main thread
// at most, and twice as long each time a slice ran late (the main thread was
// busy with the page), up to half a second; on time again, the pause halves.
static Seconds s_commitPauseHaiku { 12_ms };
static constexpr Seconds minimumCommitPauseHaiku { 12_ms };
static constexpr Seconds maximumCommitPauseHaiku { 500_ms };

static void scheduleCommitStepHaiku()
{
    if (s_commitStepScheduledHaiku || commitQueueHaiku().isEmpty())
        return;
    s_commitStepScheduledHaiku = true;
    auto due = MonotonicTime::now() + s_commitPauseHaiku;
    RunLoop::mainSingleton().dispatchAfter(s_commitPauseHaiku, [due] {
        s_commitStepScheduledHaiku = false;
        if (MonotonicTime::now() - due > 8_ms)
            s_commitPauseHaiku = std::min(s_commitPauseHaiku * 2, maximumCommitPauseHaiku);
        else
            s_commitPauseHaiku = std::max(s_commitPauseHaiku / 2, minimumCommitPauseHaiku);
        if (commitQueueHaiku().isEmpty())
            return;
        // One slice of encoding; the file is written once all is encoded.
        Ref provider = commitQueueHaiku().first();
        provider->commitCachedBytecode();
        scheduleCommitStepHaiku();
    });
}

static void queueCommitHaiku(const CachedScriptSourceProvider& provider)
{
    if (!committingProvidersHaiku().add(&provider).isNewEntry)
        return;
    commitQueueHaiku().append(provider);
    scheduleCommitStepHaiku();
}

void CachedScriptSourceProvider::commitCachedBytecode() const
{
    // JavaScriptCore asks for this when it drops the program from its code
    // cache (CodeCacheMap::pruneSlowCase), which happens while another
    // page's scripts are being compiled. Encoding every function compiled
    // since then right there (about 70 us each) held the main thread for 2.7
    // to 3.7 s on Reddit, whose scripts compile thousands (issue #20): what
    // a few milliseconds do not finish is encoded between other work.
    if (!encodePendingFunctionsHaiku(4)) {
        queueCommitHaiku(*this);
        return;
    }
    if (committingProvidersHaiku().remove(this))
        commitQueueHaiku().removeFirstMatching([this](auto& provider) { return provider.ptr() == this; });
    // Once per provider: functions compiled after this are written by the
    // next visit, which reads this file.
    encodePendingFunctionsHaiku(std::numeric_limits<double>::infinity());
    size_t functions = m_pendingFunctionsHaiku ? m_pendingFunctionsHaiku->functions.size() : 0;
    delete std::exchange(m_pendingFunctionsHaiku, nullptr);
    RefPtr bytecode = std::exchange(m_cachedBytecodeHaiku, nullptr);
    if (!bytecode || !bytecode->hasUpdates())
        return;
    auto path = bytecodeCachePathHaiku();
    if (path.isNull())
        return;
    auto start = MonotonicTime::now();
    // Another process may have replaced the file since this one read it.
    struct stat current;
    if (m_cachedBytecodeFileSizeHaiku && (stat(path.utf8().legacyCStringPointer(), &current) || static_cast<size_t>(current.st_size) != m_cachedBytecodeFileSizeHaiku))
        return;
    // Function updates change the old file's bytes as well as adding to it,
    // and processes (this one too) may be decoding from the old file as
    // mapped: the whole new file is put together here and written beside the
    // old one, then replaces it.
    size_t size = bytecode->sizeForUpdate();
    // A visit that compiled little that was not in the file already: not
    // worth rewriting it (YouTube's is 44 MB, 30 ms to put together).
    if (m_cachedBytecodeFileSizeHaiku && (size - m_cachedBytecodeFileSizeHaiku) * 20 < m_cachedBytecodeFileSizeHaiku) {
        if (traceHaiku())
            fprintf(stderr, "bytecode cache: %zu new bytes not written for %s\n", size - m_cachedBytecodeFileSizeHaiku, sourceURL().utf8().legacyCStringPointer());
        return;
    }
    auto data = MallocSpan<uint8_t>::malloc(size);
    auto file = data.mutableSpan();
    memcpySpan(file, bytecode->span());
    bool fits = true;
    bytecode->commitUpdates([&](off_t offset, std::span<const uint8_t> update) {
        if (offset < 0 || static_cast<size_t>(offset) > size || update.size() > size - offset) {
            fits = false;
            return;
        }
        memcpySpan(file.subspan(offset), update);
    });
    if (!fits)
        return;
    if (traceHaiku())
        fprintf(stderr, "bytecode cache: %zu bytes (%zu read, %zu functions, %.1f ms encoding) ready in %.1f ms for %s\n", size, m_cachedBytecodeFileSizeHaiku, functions, m_encodeMillisecondsHaiku, (MonotonicTime::now() - start).milliseconds(), sourceURL().utf8().legacyCStringPointer());
    writerQueueHaiku().dispatch([path = path.isolatedCopy(), data = WTF::move(data)] {
        auto start = MonotonicTime::now();
        auto pathUTF8 = path.utf8();
        auto temporary = makeString(path, '.', getpid(), ".tmp"_s).utf8();
        int fd = open(temporary.legacyCStringPointer(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0)
            return;
        bool written = writeAllHaiku(fd, data.span()) && !fsync(fd);
        close(fd);
        if (!written || rename(temporary.legacyCStringPointer(), pathUTF8.legacyCStringPointer())) {
            unlink(temporary.legacyCStringPointer());
            return;
        }
        if (traceHaiku())
            fprintf(stderr, "bytecode cache: wrote %zu bytes in %.1f ms to %s\n", data.span().size(), (MonotonicTime::now() - start).milliseconds(), pathUTF8.legacyCStringPointer());
    });
}

void CachedScriptSourceProvider::scheduleBytecodeCommitHaiku() const
{
    if (m_bytecodeCommitScheduledHaiku)
        return;
    m_bytecodeCommitScheduledHaiku = true;
    RunLoop::mainSingleton().dispatchAfter(commitDelayHaiku, [protectedThis = Ref { *this }] {
        queueCommitHaiku(protectedThis.get());
    });
}

// No longer used: the functions are encoded a few milliseconds at a time
// between the main loop's other work by scheduleCommitStepHaiku(), which
// calls commitCachedBytecode() for each slice.
void CachedScriptSourceProvider::commitStepHaiku() const
{
    commitCachedBytecode();
}

} // namespace WebCore

#endif // PLATFORM(HAIKU)
