/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "fs/detect.hpp"
#include "fs/fat.hpp"
#include "fs/exfat.hpp"
#include "fs/ntfs.hpp"
#include "fs/ext.hpp"

#include <cstring>
#include <vector>

namespace reflash {

const char *fs_type_name(FsType t) {
    switch (t) {
    case FsType::Fat12:
        return "fat12";
    case FsType::Fat16:
        return "fat16";
    case FsType::Fat32:
        return "fat32";
    case FsType::ExFat:
        return "exfat";
    case FsType::Ntfs:
        return "ntfs";
    case FsType::Ext2:
        return "ext2";
    case FsType::Ext3:
        return "ext3";
    case FsType::Ext4:
        return "ext4";
    default:
        return "unknown";
    }
}

FsType detect_fs(Device &dev, std::uint64_t base_offset, std::string *err) {
    unsigned char buf[4096];
    std::memset(buf, 0, sizeof buf);
    if (!dev.pread_all(buf, base_offset, sizeof buf, err))
        return FsType::Unknown;

    /* ext* superblock at offset 1024 */
    if (base_offset + 1024 + 2 <= base_offset + sizeof buf) {
        const unsigned char *sb = buf + 1024;
        std::uint16_t magic = static_cast<std::uint16_t>(sb[56]) |
                              (static_cast<std::uint16_t>(sb[57]) << 8);
        if (magic == 0xEF53) {
            std::uint32_t feat_compat =
                static_cast<std::uint32_t>(sb[92]) | (static_cast<std::uint32_t>(sb[93]) << 8) |
                (static_cast<std::uint32_t>(sb[94]) << 16) | (static_cast<std::uint32_t>(sb[95]) << 24);
            std::uint32_t feat_ro =
                static_cast<std::uint32_t>(sb[96]) | (static_cast<std::uint32_t>(sb[97]) << 8) |
                (static_cast<std::uint32_t>(sb[98]) << 16) | (static_cast<std::uint32_t>(sb[99]) << 24);
            /* EXT3 has journal; EXT4 has extents etc. Heuristic: */
            const std::uint32_t EXT3_FEATURE_COMPAT_HAS_JOURNAL = 0x0004;
            const std::uint32_t EXT4_FEATURE_RO_COMPAT_EXTENTS = 0x0040;
            if (feat_ro & EXT4_FEATURE_RO_COMPAT_EXTENTS)
                return FsType::Ext4;
            if (feat_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)
                return FsType::Ext3;
            return FsType::Ext2;
        }
    }

    /* NTFS: OEM "NTFS    " at BPB+3 */
    if (std::memcmp(buf + 3, "NTFS    ", 8) == 0)
        return FsType::Ntfs;

    /* exFAT */
    if (std::memcmp(buf + 3, "EXFAT   ", 8) == 0)
        return FsType::ExFat;

    /* FAT: check 0x55AA and OEM / fs type string */
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        char fstype[9];
        std::memcpy(fstype, buf + 82, 8);
        fstype[8] = 0;
        if (std::memcmp(fstype, "FAT32   ", 8) == 0)
            return FsType::Fat32;
        std::memcpy(fstype, buf + 54, 8);
        fstype[8] = 0;
        if (std::memcmp(fstype, "FAT16   ", 8) == 0)
            return FsType::Fat16;
        if (std::memcmp(fstype, "FAT12   ", 8) == 0)
            return FsType::Fat12;
        /* fallback by cluster count */
        std::uint16_t bps = static_cast<std::uint16_t>(buf[11]) | (static_cast<std::uint16_t>(buf[12]) << 8);
        std::uint8_t spc = buf[13];
        std::uint16_t reserved = static_cast<std::uint16_t>(buf[14]) | (static_cast<std::uint16_t>(buf[15]) << 8);
        std::uint8_t fats = buf[16];
        std::uint16_t root_ents = static_cast<std::uint16_t>(buf[17]) | (static_cast<std::uint16_t>(buf[18]) << 8);
        std::uint16_t tot16 = static_cast<std::uint16_t>(buf[19]) | (static_cast<std::uint16_t>(buf[20]) << 8);
        std::uint16_t fatsz16 = static_cast<std::uint16_t>(buf[22]) | (static_cast<std::uint16_t>(buf[23]) << 8);
        std::uint32_t tot32 = static_cast<std::uint32_t>(buf[32]) | (static_cast<std::uint32_t>(buf[33]) << 8) |
                              (static_cast<std::uint32_t>(buf[34]) << 16) | (static_cast<std::uint32_t>(buf[35]) << 24);
        std::uint32_t fatsz32 = static_cast<std::uint32_t>(buf[36]) | (static_cast<std::uint32_t>(buf[37]) << 8) |
                                (static_cast<std::uint32_t>(buf[38]) << 16) | (static_cast<std::uint32_t>(buf[39]) << 24);
        std::uint32_t totsec = tot16 ? tot16 : tot32;
        std::uint32_t fatsz = fatsz16 ? fatsz16 : fatsz32;
        if (bps && spc && fats) {
            std::uint32_t root_secs = ((root_ents * 32) + (bps - 1)) / bps;
            std::uint32_t data_secs = totsec - (reserved + fats * fatsz + root_secs);
            std::uint32_t clusters = data_secs / spc;
            if (clusters < 4085)
                return FsType::Fat12;
            if (clusters < 65525)
                return FsType::Fat16;
            return FsType::Fat32;
        }
    }

    if (err)
        *err = "unrecognized filesystem";
    return FsType::Unknown;
}

bool scan_filesystem(Device &dev, std::uint64_t base_offset, FsType type, FsScanResult &out) {
    out = FsScanResult{};
    out.type = type;
    switch (type) {
    case FsType::Fat12:
    case FsType::Fat16:
    case FsType::Fat32:
        return scan_fat(dev, base_offset, type, out);
    case FsType::ExFat:
        return scan_exfat(dev, base_offset, out);
    case FsType::Ntfs:
        return scan_ntfs(dev, base_offset, out);
    case FsType::Ext2:
    case FsType::Ext3:
    case FsType::Ext4:
        return scan_ext(dev, base_offset, type, out);
    default:
        out.error = "unsupported filesystem";
        return false;
    }
}

} /* namespace reflash */
