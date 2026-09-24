/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "io/page_cache.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace sdmsg {

namespace {

#ifndef __NR_cachestat
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) ||          \
    defined(__powerpc64__) || defined(__s390x__)
#define __NR_cachestat 451
#endif
#endif

struct cachestat_range {
    std::uint64_t off;
    std::uint64_t len;
};

struct cachestat {
    std::uint64_t nr_cache;
    std::uint64_t nr_dirty;
    std::uint64_t nr_writeback;
    std::uint64_t nr_evicted;
    std::uint64_t nr_recently_evicted;
};

bool cachestat_ok(int fd, std::uint64_t offset, std::uint64_t length, cachestat *out) {
#ifdef __NR_cachestat
    cachestat_range range{offset, length};
    long rc = ::syscall(__NR_cachestat, fd, &range, out, 0u);
    return rc == 0;
#else
    (void)fd;
    (void)offset;
    (void)length;
    (void)out;
    return false;
#endif
}

long page_size() {
    long p = ::sysconf(_SC_PAGESIZE);
    return p > 0 ? p : 4096;
}

} /* namespace */

FlushState query_flush_cachestat(int fd, std::uint64_t offset, std::uint64_t length) {
    if (fd < 0 || length == 0)
        return FlushState::Unknown;
    cachestat cs {};
    if (!cachestat_ok(fd, offset, length, &cs))
        return FlushState::Unknown;
    if (cs.nr_dirty > 0 || cs.nr_writeback > 0)
        return FlushState::Dirty;
    return FlushState::Clean;
}

FlushState query_flush_mincore(int fd, std::uint64_t offset, std::uint64_t length) {
    if (fd < 0 || length == 0)
        return FlushState::Unknown;
    const std::uint64_t page = static_cast<std::uint64_t>(page_size());
    std::uint64_t map_off = offset & ~(page - 1);
    std::uint64_t map_end = (offset + length + page - 1) & ~(page - 1);
    if (map_end <= map_off)
        return FlushState::Unknown;
    std::size_t map_len = static_cast<std::size_t>(map_end - map_off);
    void *addr = ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(map_off));
    if (addr == MAP_FAILED)
        return FlushState::Unknown;
    std::size_t npages = map_len / static_cast<std::size_t>(page);
    std::vector<unsigned char> vec(npages);
    int rc = ::mincore(addr, map_len, vec.data());
    ::munmap(addr, map_len);
    if (rc != 0)
        return FlushState::Unknown;
    /* Pages overlapping the requested byte range. */
    std::uint64_t first = (offset - map_off) / page;
    std::uint64_t last = (offset + length - 1 - map_off) / page;
    for (std::uint64_t i = first; i <= last && i < npages; ++i) {
        if (vec[static_cast<std::size_t>(i)] & 1)
            return FlushState::Dirty; /* still resident */
    }
    return FlushState::Clean;
}

FlushState query_flush_state(int fd, std::uint64_t offset, std::uint64_t length, FlushProbe probe) {
    if (probe == FlushProbe::Mincore)
        return query_flush_mincore(fd, offset, length);
    return query_flush_cachestat(fd, offset, length);
}

bool kick_writeback(int fd, std::uint64_t offset, std::uint64_t length, std::string *err) {
    if (fd < 0 || length == 0)
        return true;
#ifdef SYNC_FILE_RANGE_WRITE
    if (::sync_file_range(fd, static_cast<off64_t>(offset), static_cast<off64_t>(length),
                          SYNC_FILE_RANGE_WRITE) == 0)
        return true;
    if (err)
        *err = strerror(errno);
    return false;
#else
    (void)err;
    return true;
#endif
}

bool wait_flushed(int fd, std::uint64_t offset, std::uint64_t length, std::string *err) {
    if (fd < 0 || length == 0)
        return true;
#ifdef SYNC_FILE_RANGE_WAIT_AFTER
    unsigned flags = SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WAIT_AFTER;
    if (::sync_file_range(fd, static_cast<off64_t>(offset), static_cast<off64_t>(length), flags) == 0)
        return true;
#endif
    if (::fdatasync(fd) == 0)
        return true;
    if (err)
        *err = strerror(errno);
    return false;
}

} /* namespace sdmsg */
