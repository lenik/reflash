/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "io/partition.hpp"
#include "io/device.hpp"

#include <cstring>
#include <vector>

namespace sdmsg {

namespace {

std::uint32_t le32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const unsigned char *p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

} /* namespace */

bool probe_partitions(Device &dev, PartitionMap &out, std::string *err) {
    out = PartitionMap{};
    out.sector_size = dev.sector_size() ? dev.sector_size() : 512;

    std::vector<unsigned char> sector(out.sector_size, 0);
    if (!dev.pread_all(sector.data(), 0, sector.size(), err))
        return false;

    /* GPT protective MBR often has type 0xEE in first entry. */
    bool gpt_hint = false;
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        unsigned char type = sector[446 + 4];
        if (type == 0xEE)
            gpt_hint = true;
    }

    if (gpt_hint || true) {
        /* Read LBA 1 for GPT header */
        std::vector<unsigned char> hdr(out.sector_size, 0);
        if (dev.pread_all(hdr.data(), out.sector_size, hdr.size(), nullptr) &&
            std::memcmp(hdr.data(), "EFI PART", 8) == 0) {
            out.kind = PartitionTableKind::Gpt;
            std::uint64_t part_lba = le64(hdr.data() + 72);
            std::uint32_t part_count = le32(hdr.data() + 80);
            std::uint32_t part_size = le32(hdr.data() + 84);
            if (part_size < 128)
                part_size = 128;
            std::uint64_t table_end =
                (part_lba + ((part_count * part_size + out.sector_size - 1) / out.sector_size)) *
                out.sector_size;
            out.table_bytes = table_end;

            std::uint64_t bytes = static_cast<std::uint64_t>(part_count) * part_size;
            std::vector<unsigned char> ents(bytes);
            if (!dev.pread_all(ents.data(), part_lba * out.sector_size, ents.size(), err))
                return false;

            int idx = 1;
            for (std::uint32_t i = 0; i < part_count; ++i) {
                const unsigned char *e = ents.data() + i * part_size;
                /* type GUID all zero => unused */
                bool empty = true;
                for (int k = 0; k < 16; ++k) {
                    if (e[k] != 0) {
                        empty = false;
                        break;
                    }
                }
                if (empty)
                    continue;
                Partition p;
                p.index = idx++;
                p.start_lba = le64(e + 32);
                std::uint64_t end_lba = le64(e + 40);
                p.length_lba = end_lba >= p.start_lba ? (end_lba - p.start_lba + 1) : 0;
                p.start_bytes = p.start_lba * out.sector_size;
                p.length_bytes = p.length_lba * out.sector_size;
                p.name = "::part-" + std::to_string(p.index);
                out.parts.push_back(p);
            }
            return true;
        }
    }

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        out.kind = PartitionTableKind::Mbr;
        out.table_bytes = out.sector_size; /* classic MBR */
        int idx = 1;
        for (int i = 0; i < 4; ++i) {
            const unsigned char *e = sector.data() + 446 + i * 16;
            unsigned char type = e[4];
            if (type == 0)
                continue;
            Partition p;
            p.index = idx++;
            p.start_lba = le32(e + 8);
            p.length_lba = le32(e + 12);
            p.start_bytes = p.start_lba * out.sector_size;
            p.length_bytes = p.length_lba * out.sector_size;
            p.name = "::part-" + std::to_string(p.index);
            out.parts.push_back(p);
        }
        return true;
    }

    out.kind = PartitionTableKind::None;
    out.table_bytes = 0;
    return true;
}

} /* namespace sdmsg */
