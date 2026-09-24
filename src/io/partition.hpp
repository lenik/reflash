/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reflash {

struct Partition {
    int index = 0; /* 1-based */
    std::uint64_t start_lba = 0;
    std::uint64_t length_lba = 0;
    std::uint64_t start_bytes = 0;
    std::uint64_t length_bytes = 0;
    std::string name; /* ::part-N */
};

enum class PartitionTableKind { None, Mbr, Gpt };

struct PartitionMap {
    PartitionTableKind kind = PartitionTableKind::None;
    std::uint64_t sector_size = 512;
    std::uint64_t table_bytes = 0; /* region covering MBR or GPT header+entries */
    std::vector<Partition> parts;
};

/* Probe first sectors of an already-opened device (or raw image). */
bool probe_partitions(class Device &dev, PartitionMap &out, std::string *err = nullptr);

} /* namespace reflash */
