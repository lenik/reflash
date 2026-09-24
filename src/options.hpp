/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>

namespace reflash {

enum class RunMode { Linear, Recursive };
enum class Action { Massage, Test };

/* How rewrite interacts with the page cache / flush detection. */
enum class WriteCacheMode : std::uint8_t {
    WriteThrough = 0, /* O_DIRECT / fdatasync; cells go green immediately */
    Cachestat = 1,    /* default: poll dirty/writeback every ~0.78s */
    Mincore = 2,      /* poll page residency via mmap+mincore every ~0.78s */
};

struct Options {
    std::string target;
    std::string sqlite_db;
    /* True when -d/--sqlite-db was given on the command line. */
    bool sqlite_db_explicit = false;
    std::uint64_t block_size = 0; /* 0 = auto */
    RunMode mode = RunMode::Linear;
    Action action = Action::Massage;
    /* GUI: read the scan, do not write blocks back. */
    bool dry_run = false;
    WriteCacheMode write_cache = WriteCacheMode::Cachestat;
    /* Re-read via O_DIRECT after write; never compare against page cache. */
    bool verify_writes = false;
    /* Poll interval for Cached→Ok (ms); default ~0.78s. */
    int flush_poll_ms = 780;
    /* SHA-1 of an unchanged file stays valid this many days (default ~6 months). */
    int sha1_valid_days = 183;
    bool gui = false;
    int verbosity = 0; /* -quiet .. +verbose */

    bool write_through() const { return write_cache == WriteCacheMode::WriteThrough; }
};

bool parse_options(int argc, char **argv, Options &out);
void print_usage(FILE *out);
void print_version();

} /* namespace reflash */
