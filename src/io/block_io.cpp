/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "io/block_io.hpp"

#include <algorithm>
#include <vector>

namespace sdmsg {

BlockIoResult rewrite_range(Device &dev, std::uint64_t offset, std::uint64_t length,
                            std::uint64_t block_size, bool verify_only, PauseControl &pause,
                            Progress &progress, const std::string &object_path) {
    BlockIoResult result;
    if (block_size == 0)
        block_size = 4096;

    std::vector<unsigned char> buf(block_size);
    std::uint64_t pos = offset;
    std::uint64_t end = offset + length;

    while (pos < end) {
        if (!pause.wait_if_paused()) {
            result.ok = false;
            return result;
        }

        std::uint64_t chunk = std::min(block_size, end - pos);
        std::string err;
        if (!dev.pread_all(buf.data(), pos, static_cast<std::size_t>(chunk), &err)) {
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

        if (!verify_only) {
            if (!dev.pwrite_all(buf.data(), pos, static_cast<std::size_t>(chunk), &err)) {
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
        }

        progress.mark_cell(pos, chunk, CellStatus::Ok);
        progress.add_done(chunk);
        pos += chunk;
    }

    return result;
}

} /* namespace sdmsg */
