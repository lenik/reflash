/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/pause.hpp"
#include "engine/progress.hpp"
#include "io/device.hpp"
#include "io/page_cache.hpp"
#include "options.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sdmsg {

enum class ExtentStatus { Ok = 0, ReadErr = 1, WriteErr = 2 };

struct RewriteExtent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    ExtentStatus status = ExtentStatus::Ok;
};

using Sha1Callback = std::function<void(const unsigned char digest[20])>;

struct BlockIoResult {
    bool ok = true;
    std::vector<RewriteExtent> errors;
};

struct RewriteFlags {
    bool verify_only = false;
    WriteCacheMode write_cache = WriteCacheMode::Cachestat;
    bool verify_writes = false;
    /* Poll Cached→Ok every this many ms (default 780). */
    int flush_poll_ms = 780;
};

/*
 * Read-each-block then write-back same bytes over [offset, offset+length).
 * WriteThrough: O_DIRECT/fdatasync, cells → Ok.
 * Cachestat/Mincore: cells → Cached, poll flush every ~0.78s → Ok.
 */
BlockIoResult rewrite_range(Device &dev, std::uint64_t offset, std::uint64_t length,
                            std::uint64_t block_size, RewriteFlags flags, PauseControl &pause,
                            Progress &progress, const std::string &object_path);

} /* namespace sdmsg */
