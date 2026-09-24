/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
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

namespace reflash {

enum class FsType { Unknown, Fat12, Fat16, Fat32, ExFat, Ntfs, Ext2, Ext3, Ext4 };

struct FsFileExtent {
    std::uint64_t offset = 0; /* absolute byte offset on device/image */
    std::uint64_t length = 0;
};

struct FsObject {
    std::string path; /* UTF-8 path or ::special */
    std::uint64_t size = 0;
    std::vector<FsFileExtent> extents;
};

struct FsScanResult {
    FsType type = FsType::Unknown;
    std::string label;
    std::vector<FsObject> objects;
    std::string error;
};

const char *fs_type_name(FsType t);
FsType detect_fs(Device &dev, std::uint64_t base_offset, std::string *err = nullptr);

/* Scan filesystem at base_offset (0 for whole device/file or partition start). */
bool scan_filesystem(Device &dev, std::uint64_t base_offset, FsType type, FsScanResult &out);

} /* namespace reflash */
