/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "io/block_io.hpp"
#include "io/page_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sdmsg {

namespace {

struct AlignedBuf {
    unsigned char *p = nullptr;
    std::size_t bytes = 0;

    explicit AlignedBuf(std::size_t align, std::size_t size) {
        if (align < sizeof(void *))
            align = sizeof(void *);
        if (posix_memalign(reinterpret_cast<void **>(&p), align, size) != 0)
            p = nullptr;
        else
            bytes = size;
    }
    ~AlignedBuf() { std::free(p); }
    AlignedBuf(const AlignedBuf &) = delete;
    AlignedBuf &operator=(const AlignedBuf &) = delete;
    explicit operator bool() const { return p != nullptr; }
};

bool verify_against_media(Device &dev, const unsigned char *expect, std::uint64_t offset,
                          std::size_t len, AlignedBuf &scratch, std::string *err) {
    if (!dev.has_direct()) {
        if (err)
            *err = "O_DIRECT unavailable; cannot verify without page cache";
        return false;
    }
    if (!scratch || scratch.bytes < len) {
        if (err)
            *err = "verify buffer too small";
        return false;
    }
    if (!dev.pread_direct(scratch.p, offset, len, err))
        return false;
    if (std::memcmp(expect, scratch.p, len) != 0) {
        if (err)
            *err = "verify mismatch (media != written)";
        return false;
    }
    return true;
}

using clock = std::chrono::steady_clock;

bool maybe_promote(Device &dev, FlushProbe probe, std::uint64_t from, std::uint64_t to,
                   Progress &progress, clock::time_point *last_poll, double poll_sec, bool force) {
    if (to <= from)
        return false;
    auto now = clock::now();
    double dt = std::chrono::duration<double>(now - *last_poll).count();
    if (!force && dt < poll_sec)
        return false;
    *last_poll = now;
    FlushState st = query_flush_state(dev.fd(), from, to - from, probe);
    if (st == FlushState::Clean) {
        progress.mark_cell(from, to - from, CellStatus::Ok);
        return true;
    }
    return false;
}

} /* namespace */

BlockIoResult rewrite_range(Device &dev, std::uint64_t offset, std::uint64_t length,
                            std::uint64_t block_size, RewriteFlags flags, PauseControl &pause,
                            Progress &progress, const std::string &object_path) {
    BlockIoResult result;
    if (block_size == 0)
        block_size = 4096;

    const bool write_through = flags.write_cache == WriteCacheMode::WriteThrough;
    const FlushProbe probe = flags.write_cache == WriteCacheMode::Mincore ? FlushProbe::Mincore
                                                                         : FlushProbe::Cachestat;

    std::uint64_t align = dev.direct_align();
    if (align == 0)
        align = 4096;
    bool need_direct = (write_through || flags.verify_writes) && !flags.verify_only;
    if (need_direct && block_size % align != 0)
        block_size = ((block_size + align - 1) / align) * align;

    AlignedBuf buf(static_cast<std::size_t>(align), static_cast<std::size_t>(block_size));
    AlignedBuf verify_buf(static_cast<std::size_t>(align), static_cast<std::size_t>(block_size));
    if (!buf) {
        result.ok = false;
        progress.message(LogLevel::Error, object_path + ": aligned buffer alloc failed");
        return result;
    }
    if (flags.verify_writes && !flags.verify_only && !verify_buf) {
        result.ok = false;
        progress.message(LogLevel::Error, object_path + ": verify buffer alloc failed");
        return result;
    }
    if (flags.verify_writes && !flags.verify_only && !dev.has_direct()) {
        result.ok = false;
        progress.message(LogLevel::Error,
                         object_path + ": Verify writes needs O_DIRECT (unavailable on this target)");
        return result;
    }

    std::uint64_t pos = offset;
    std::uint64_t end = offset + length;
    std::uint64_t cached_from = 0;
    std::uint64_t cached_to = 0;
    bool have_cached = false;
    auto last_poll = clock::now();
    const double poll_sec =
        (flags.flush_poll_ms > 0 ? static_cast<double>(flags.flush_poll_ms) : 780.0) / 1000.0;

    while (pos < end) {
        if (!pause.wait_if_paused()) {
            result.ok = false;
            return result;
        }

        std::uint64_t chunk = std::min(block_size, end - pos);
        if (need_direct && chunk % align != 0 && pos + chunk < end)
            chunk -= chunk % align;
        if (chunk == 0)
            break;

        std::string err;
        if (!dev.pread_all(buf.p, pos, static_cast<std::size_t>(chunk), &err)) {
            result.ok = false;
            RewriteExtent e{pos, chunk, ExtentStatus::ReadErr};
            result.errors.push_back(e);
            progress.log(pos, chunk, CellStatus::ReadErr,
                         object_path + ": read error at " + std::to_string(pos) + " +" +
                             std::to_string(chunk) + ": " + err);
            pos += chunk;
            progress.add_done(chunk);
            continue;
        }

        if (!flags.verify_only) {
            bool wrote = false;
            if (write_through) {
                if (dev.has_direct() && (pos % align == 0) && (chunk % align == 0))
                    wrote = dev.pwrite_direct(buf.p, pos, static_cast<std::size_t>(chunk), &err);
                else
                    wrote = dev.pwrite_sync(buf.p, pos, static_cast<std::size_t>(chunk), &err);
            } else {
                wrote = dev.pwrite_all(buf.p, pos, static_cast<std::size_t>(chunk), &err);
            }

            if (!wrote) {
                result.ok = false;
                RewriteExtent e{pos, chunk, ExtentStatus::WriteErr};
                result.errors.push_back(e);
                progress.log(pos, chunk, CellStatus::WriteErr,
                             object_path + ": write error at " + std::to_string(pos) + " +" +
                                 std::to_string(chunk) + ": " + err);
                pos += chunk;
                progress.add_done(chunk);
                continue;
            }

            if (flags.verify_writes) {
                if (!write_through)
                    wait_flushed(dev.fd(), pos, chunk, nullptr);
                std::string verr;
                if ((pos % align) != 0 || (chunk % align) != 0)
                    verr = "unaligned for O_DIRECT verify";
                else if (!verify_against_media(dev, buf.p, pos, static_cast<std::size_t>(chunk),
                                               verify_buf, &verr)) {
                    /* verr set */
                } else {
                    verr.clear();
                }
                if (!verr.empty()) {
                    result.ok = false;
                    RewriteExtent e{pos, chunk, ExtentStatus::WriteErr};
                    result.errors.push_back(e);
                    progress.log(pos, chunk, CellStatus::WriteErr,
                                 object_path + ": verify failed at " + std::to_string(pos) + " +" +
                                     std::to_string(chunk) + ": " + verr);
                    pos += chunk;
                    progress.add_done(chunk);
                    continue;
                }
            }

            if (write_through || flags.verify_writes) {
                progress.mark_cell(pos, chunk, CellStatus::Ok);
            } else {
                progress.mark_cell(pos, chunk, CellStatus::Cached);
                if (!have_cached) {
                    cached_from = pos;
                    cached_to = pos + chunk;
                    have_cached = true;
                } else {
                    if (pos < cached_from)
                        cached_from = pos;
                    if (pos + chunk > cached_to)
                        cached_to = pos + chunk;
                }
                if (maybe_promote(dev, probe, cached_from, cached_to, progress, &last_poll, poll_sec,
                                  false))
                    have_cached = false;
            }
        } else {
            progress.mark_cell(pos, chunk, CellStatus::Ok);
        }

        progress.add_done(chunk);
        pos += chunk;
    }

    if (have_cached)
        maybe_promote(dev, probe, cached_from, cached_to, progress, &last_poll, poll_sec, true);
    return result;
}

} /* namespace sdmsg */
