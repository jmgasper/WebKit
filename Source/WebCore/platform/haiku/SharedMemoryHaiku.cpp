/*
 * Copyright (C) 2019 Haiku, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SharedMemory.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/RefPtr.h>

namespace WebCore {

SharedMemoryHandle::SharedMemoryHandle(const SharedMemoryHandle& other)
    : m_handle(other.m_handle.duplicate())
    , m_size(other.m_size)
{
}

UnixFileDescriptor SharedMemoryHandle::releaseHandle()
{
    return WTF::move(m_handle);
}

RefPtr<SharedMemory> SharedMemory::allocate(size_t size)
{
    if (!size || size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return nullptr;

    // Keep separate read/write and read-only capabilities before removing the
    // name. dup() cannot reduce a descriptor's access mode after unlinking.
    UnixFileDescriptor descriptor;
    UnixFileDescriptor readOnlyDescriptor;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        char name[96];
        std::snprintf(name, sizeof(name), "/WebKitSharedMemory.%ld.%08x.%08x", static_cast<long>(getpid()),
            cryptographicallyRandomNumber<unsigned>(), cryptographicallyRandomNumber<unsigned>());
        int file;
        do {
            file = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
        } while (file < 0 && errno == EINTR);
        if (file < 0) {
            if (errno == EEXIST)
                continue;
            return nullptr;
        }
        descriptor = UnixFileDescriptor { file, UnixFileDescriptor::Adopt };
        int readOnlyFile;
        do {
            readOnlyFile = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
        } while (readOnlyFile < 0 && errno == EINTR);
        readOnlyDescriptor = UnixFileDescriptor { readOnlyFile, UnixFileDescriptor::Adopt };
        int result;
        do {
            result = shm_unlink(name);
        } while (result < 0 && errno == EINTR);
        if (result || !readOnlyDescriptor)
            return nullptr;
        break;
    }
    if (!descriptor)
        return nullptr;

    while (ftruncate(descriptor.value(), size)) {
        if (errno != EINTR)
            return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor.value(), 0);
    if (data == MAP_FAILED)
        return nullptr;
    RefPtr<SharedMemory> memory = adoptRef(new SharedMemory);
    memory->m_size = size;
    memory->m_data = data;
    memory->m_fileDescriptor = WTF::move(descriptor);
    memory->m_readOnlyFileDescriptor = WTF::move(readOnlyDescriptor);
    return memory;
}

RefPtr<SharedMemory> SharedMemory::map(Handle&& handle, Protection protection, CopyOnWrite copyOnWrite)
{
    if (!handle.m_handle || !handle.m_size || handle.m_size > std::numeric_limits<size_t>::max())
        return nullptr;
    struct stat info;
    if (fstat(handle.m_handle.value(), &info) || !S_ISREG(info.st_mode)
        || info.st_size < 0 || handle.m_size > static_cast<uint64_t>(info.st_size))
        return nullptr;

    int access = PROT_READ | (protection == Protection::ReadWrite ? PROT_WRITE : 0);
    int flags = copyOnWrite == CopyOnWrite::Yes ? MAP_PRIVATE : MAP_SHARED;
    void* data = mmap(nullptr, handle.size(), access, flags, handle.m_handle.value(), 0);
    if (data == MAP_FAILED)
        return nullptr;
    RefPtr<SharedMemory> memory = adoptRef(new SharedMemory);
    memory->m_size = handle.size();
    memory->m_data = data;
    memory->m_fileDescriptor = WTF::move(handle.m_handle);
    memory->m_isCopyOnWrite = copyOnWrite == CopyOnWrite::Yes;
    if ((fcntl(memory->m_fileDescriptor.value(), F_GETFL) & O_ACCMODE) == O_RDONLY)
        memory->m_readOnlyFileDescriptor = memory->m_fileDescriptor.duplicate();
    return memory;
}

RefPtr<SharedMemory> SharedMemory::wrapMap(void* data, size_t size, int descriptor)
{
    if (!data || data == MAP_FAILED || !size || descriptor < 0)
        return nullptr;
    RefPtr<SharedMemory> memory = adoptRef(new SharedMemory);
    memory->m_size = size;
    memory->m_data = data;
    memory->m_fileDescriptor = UnixFileDescriptor { descriptor, UnixFileDescriptor::Duplicate };
    memory->m_isWrappingMap = true;
    if (!memory->m_fileDescriptor)
        return nullptr;
    if ((fcntl(memory->m_fileDescriptor.value(), F_GETFL) & O_ACCMODE) == O_RDONLY)
        memory->m_readOnlyFileDescriptor = memory->m_fileDescriptor.duplicate();
    return memory;
}

SharedMemory::~SharedMemory()
{
    if (!m_isWrappingMap)
        munmap(m_data, m_size);
}

std::optional<SharedMemory::Handle> SharedMemory::createHandle(Protection protection)
{
    // A descriptor for a private mapping refers to the original file, not its
    // private writes. Export those bytes through a new backing object instead.
    if (m_isCopyOnWrite)
        return Handle::createCopy(span(), protection);
    // An imported writable descriptor has no separate read-only capability.
    // Decline that conversion instead of exporting writable access as read-only.
    auto& descriptor = protection == Protection::ReadOnly ? m_readOnlyFileDescriptor : m_fileDescriptor;
    auto duplicate = descriptor.duplicate();
    if (!duplicate)
        return std::nullopt;
    if (protection == Protection::ReadWrite && (fcntl(duplicate.value(), F_GETFL) & O_ACCMODE) != O_RDWR)
        return std::nullopt;
    return Handle { WTF::move(duplicate), m_size };
}

area_id SharedMemory::area() const
{
    // Only bitmap sharing opts into Haiku's app_server area transport. Ordinary
    // IPC memory remains uncloneable by unrelated teams and uses descriptors.
    area_id id = area_for(m_data);
    area_info info;
    if (id < 0 || get_area_info(id, &info) != B_OK)
        return B_BAD_VALUE;
    auto protection = info.protection & (B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA);
    if (set_area_protection(id, protection | B_CLONEABLE_AREA) != B_OK)
        return B_NOT_ALLOWED;
    return id;
}

} // namespace WebCore
