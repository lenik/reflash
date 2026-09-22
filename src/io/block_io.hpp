/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/pause.hpp"
#include "engine/progress.hpp"
#include "io/device.hpp"

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

/*
 * Read-each-block then write-back same bytes over [offset, offset+length).
 * Pausable only between blocks. Does not truncate. Does not compute SHA-1
 * (that happens after remount via the VFS).
 * If verify_only, only read (no write) — used for dry I/O checks.
 */
BlockIoResult rewrite_range(Device &dev, std::uint64_t offset, std::uint64_t length,
                            std::uint64_t block_size, bool verify_only, PauseControl &pause,
                            Progress &progress, const std::string &object_path);

} /* namespace sdmsg */
