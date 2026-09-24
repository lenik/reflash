/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "fs/exfat.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

namespace reflash {

namespace {

std::uint16_t r16(const unsigned char *p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint32_t r32(const unsigned char *p) {
    return static_cast<std::uint32_t>(r16(p)) | (static_cast<std::uint32_t>(r16(p + 2)) << 16);
}
std::uint64_t r64(const unsigned char *p) {
    return static_cast<std::uint64_t>(r32(p)) | (static_cast<std::uint64_t>(r32(p + 4)) << 32);
}

struct ExGeom {
    std::uint64_t base = 0;
    std::uint16_t bps = 512;
    std::uint8_t spc_shift = 0;
    std::uint32_t fat_offset = 0; /* sectors */
    std::uint32_t fat_length = 0;
    std::uint32_t cluster_heap = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t root_cluster = 0;
};

std::uint64_t cluster_off(const ExGeom &g, std::uint32_t c) {
    std::uint64_t cluster_bytes = static_cast<std::uint64_t>(g.bps) << g.spc_shift;
    return g.base + static_cast<std::uint64_t>(g.cluster_heap) * g.bps +
           static_cast<std::uint64_t>(c - 2) * cluster_bytes;
}

std::uint32_t fat_entry(const std::vector<unsigned char> &fat, std::uint32_t c) {
    std::uint32_t off = c * 4;
    if (off + 4 > fat.size())
        return 0xFFFFFFFF;
    return r32(fat.data() + off);
}

std::vector<FsFileExtent> chain(const ExGeom &g, const std::vector<unsigned char> &fat,
                                std::uint32_t start, std::uint64_t size, bool no_fat_chain,
                                bool whole_chain) {
    std::vector<FsFileExtent> exts;
    std::uint64_t cluster_bytes = static_cast<std::uint64_t>(g.bps) << g.spc_shift;
    if (no_fat_chain) {
        std::uint64_t left = whole_chain ? (std::numeric_limits<std::uint64_t>::max() / 2) : size;
        if (!whole_chain && left == 0)
            left = cluster_bytes;
        std::uint32_t c = start;
        std::uint64_t max_clusters = g.cluster_count + 2;
        while (left > 0 && c >= 2 && c < max_clusters) {
            std::uint64_t take = whole_chain ? cluster_bytes : std::min(left, cluster_bytes);
            FsFileExtent e{cluster_off(g, c), take};
            if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
                exts.back().length += e.length;
            else
                exts.push_back(e);
            if (!whole_chain)
                left -= take;
            else if (exts.size() > 100000)
                break;
            c++;
            if (whole_chain && size && (exts.size() * cluster_bytes >= size + cluster_bytes))
                break;
        }
        return exts;
    }
    std::uint64_t left =
        whole_chain || size == 0 ? std::numeric_limits<std::uint64_t>::max() : size;
    std::uint32_t c = start;
    std::set<std::uint32_t> seen;
    while (c >= 2 && c < 0xFFFFFFF8 && left > 0) {
        if (!seen.insert(c).second)
            break;
        std::uint64_t take = whole_chain ? cluster_bytes : std::min(left, cluster_bytes);
        FsFileExtent e{cluster_off(g, c), take};
        if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
            exts.back().length += e.length;
        else
            exts.push_back(e);
        if (!whole_chain)
            left -= take;
        std::uint32_t next = fat_entry(fat, c);
        if (next >= 0xFFFFFFF8)
            break;
        c = next;
    }
    return exts;
}

bool walk(Device &dev, ExGeom &g, const std::vector<unsigned char> &fat, std::uint32_t dir_clus,
          const std::string &prefix, FsScanResult &out) {
    auto dir_exts = chain(g, fat, dir_clus, 0, false, true);
    (void)dir_exts;
    std::vector<unsigned char> dir;
    for (const auto &e : dir_exts) {
        /* read whole clusters for directory */
        std::uint64_t cluster_bytes = static_cast<std::uint64_t>(g.bps) << g.spc_shift;
        std::vector<unsigned char> chunk(static_cast<size_t>(cluster_bytes));
        /* re-read by cluster for dirs when size unknown */
        (void)e;
    }
    /* Simpler: follow FAT and read each cluster fully for directory */
    {
        std::uint32_t c = dir_clus;
        std::set<std::uint32_t> seen;
        std::uint64_t cluster_bytes = static_cast<std::uint64_t>(g.bps) << g.spc_shift;
        while (c >= 2 && c < 0xFFFFFFF8) {
            if (!seen.insert(c).second)
                break;
            std::vector<unsigned char> chunk(static_cast<size_t>(cluster_bytes));
            std::string err;
            if (!dev.pread_all(chunk.data(), cluster_off(g, c), chunk.size(), &err)) {
                out.error = err;
                return false;
            }
            dir.insert(dir.end(), chunk.begin(), chunk.end());
            std::uint32_t next = fat_entry(fat, c);
            if (next >= 0xFFFFFFF8)
                break;
            c = next;
        }
    }

    for (size_t i = 0; i + 32 <= dir.size();) {
        unsigned char type = dir[i];
        if (type == 0x00)
            break;
        if (type == 0x85) { /* file entry */
            unsigned char secondary = dir[i + 1];
            bool is_dir = (dir[i + 4] & 0x10) != 0;
            size_t base_i = i;
            i += 32;
            if (i + 32 > dir.size() || dir[i] != 0xC0) {
                continue;
            }
            bool no_fat = (dir[i + 1] & 0x02) != 0;
            std::uint64_t valid_size = r64(dir.data() + i + 8);
            std::uint32_t first = r32(dir.data() + i + 20);
            i += 32;
            std::string name;
            /* secondary count includes the stream entry we already consumed. */
            for (unsigned s = 1; s < secondary && i + 32 <= dir.size(); ++s) {
                if ((dir[i] & 0x7F) != 0x41) { /* 0xC1 in-use or 0x41 deleted */
                    i += 32;
                    continue;
                }
                for (int k = 2; k < 32; k += 2) {
                    std::uint16_t ch = r16(dir.data() + i + k);
                    if (ch == 0)
                        break;
                    if (ch < 0x80)
                        name.push_back(static_cast<char>(ch));
                    else if (ch < 0x800) {
                        name.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                        name.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    } else {
                        name.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                        name.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                        name.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    }
                }
                i += 32;
            }
            if (name.empty())
                name = "unnamed";
            std::string path = prefix + "/" + name;
            FsObject obj;
            obj.path = path;
            obj.size = is_dir ? 0 : valid_size;
            if (first >= 2)
                obj.extents = chain(g, fat, first, is_dir ? 0 : valid_size, no_fat, is_dir);
            out.objects.push_back(obj);
            if (is_dir && first >= 2) {
                if (!walk(dev, g, fat, first, path, out))
                    return false;
            }
            (void)base_i;
            continue;
        }
        i += 32;
    }
    return true;
}

} /* namespace */

bool scan_exfat(Device &dev, std::uint64_t base, FsScanResult &out) {
    unsigned char boot[512];
    std::string err;
    if (!dev.pread_all(boot, base, sizeof boot, &err)) {
        out.error = err;
        return false;
    }
    if (std::memcmp(boot + 3, "EXFAT   ", 8) != 0) {
        out.error = "not exFAT";
        return false;
    }

    ExGeom g;
    g.base = base;
    std::uint8_t bps_shift = boot[108];
    g.bps = static_cast<std::uint16_t>(1u << bps_shift);
    g.spc_shift = boot[109];
    g.fat_offset = r32(boot + 80);
    g.fat_length = r32(boot + 84);
    g.cluster_heap = r32(boot + 88);
    g.cluster_count = r32(boot + 92);
    g.root_cluster = r32(boot + 96);

    FsObject boot_obj;
    boot_obj.path = "::boot";
    boot_obj.size = 24 * g.bps; /* boot region typically 24 sectors */
    boot_obj.extents.push_back({base, boot_obj.size});
    out.objects.push_back(boot_obj);

    std::vector<unsigned char> fat(static_cast<size_t>(g.fat_length) * g.bps);
    if (!dev.pread_all(fat.data(), base + static_cast<std::uint64_t>(g.fat_offset) * g.bps, fat.size(),
                       &err)) {
        out.error = err;
        return false;
    }
    FsObject fat_obj;
    fat_obj.path = "::fat";
    fat_obj.size = fat.size();
    fat_obj.extents.push_back({base + static_cast<std::uint64_t>(g.fat_offset) * g.bps, fat_obj.size});
    out.objects.push_back(fat_obj);

    return walk(dev, g, fat, g.root_cluster, "", out);
}

} /* namespace reflash */
