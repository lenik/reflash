/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>

namespace reflash {

enum class FlushState : std::uint8_t {
    /* Still dirty/writeback, or (mincore) still resident. */
    Dirty = 0,
    /* Flushed (cachestat) or no longer resident (mincore). */
    Clean = 1,
    /* Probe unavailable. */
    Unknown = 2,
};

enum class FlushProbe : std::uint8_t {
    Cachestat = 0, /* dirty/writeback via cachestat(2) — default */
    Mincore = 1,   /* page residency via mmap + mincore(2) */
};

/* cachestat: Clean iff nr_dirty+nr_writeback == 0. */
FlushState query_flush_cachestat(int fd, std::uint64_t offset, std::uint64_t length);

/* mincore: Clean iff no page in range is resident (may stay Dirty while clean-cached). */
FlushState query_flush_mincore(int fd, std::uint64_t offset, std::uint64_t length);

FlushState query_flush_state(int fd, std::uint64_t offset, std::uint64_t length, FlushProbe probe);

/* Ask the kernel to start writeback for the range (non-blocking). */
bool kick_writeback(int fd, std::uint64_t offset, std::uint64_t length, std::string *err = nullptr);

/* Wait until the range has been written back to the device. */
bool wait_flushed(int fd, std::uint64_t offset, std::uint64_t length, std::string *err = nullptr);

} /* namespace reflash */
