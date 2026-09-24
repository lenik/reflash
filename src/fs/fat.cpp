/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "fs/fat.hpp"

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

struct FatGeom {
    FsType type = FsType::Fat16;
    std::uint16_t bps = 512;
    std::uint8_t spc = 1;
    std::uint16_t reserved = 1;
    std::uint8_t fats = 2;
    std::uint32_t fatsz = 0;
    std::uint32_t root_ents = 0;
    std::uint32_t root_clus = 0;
    std::uint32_t totsec = 0;
    std::uint32_t data_start = 0; /* sector */
    std::uint32_t fat_start = 0;
    std::uint32_t root_secs = 0;
    std::uint32_t clusters = 0;
    std::uint64_t base = 0;
};

std::uint32_t fat_get(const std::vector<unsigned char> &fat, FsType type, std::uint32_t cluster) {
    if (type == FsType::Fat32) {
        std::uint32_t off = cluster * 4;
        if (off + 4 > fat.size())
            return 0x0FFFFFFF;
        return r32(fat.data() + off) & 0x0FFFFFFF;
    }
    if (type == FsType::Fat16) {
        std::uint32_t off = cluster * 2;
        if (off + 2 > fat.size())
            return 0xFFFF;
        return r16(fat.data() + off);
    }
    /* FAT12 */
    std::uint32_t off = cluster + cluster / 2;
    if (off + 1 >= fat.size())
        return 0xFFF;
    std::uint16_t val = r16(fat.data() + off);
    if (cluster & 1)
        return val >> 4;
    return val & 0x0FFF;
}

bool is_eoc(FsType type, std::uint32_t v) {
    if (type == FsType::Fat32)
        return v >= 0x0FFFFFF8;
    if (type == FsType::Fat16)
        return v >= 0xFFF8;
    return v >= 0xFF8;
}

std::uint64_t cluster_offset(const FatGeom &g, std::uint32_t cluster) {
    return g.base + (static_cast<std::uint64_t>(g.data_start) +
                     static_cast<std::uint64_t>(cluster - 2) * g.spc) *
                        g.bps;
}

std::vector<FsFileExtent> chain_extents(const FatGeom &g, const std::vector<unsigned char> &fat,
                                        std::uint32_t start, std::uint64_t file_size,
                                        bool whole_chain) {
    std::vector<FsFileExtent> exts;
    /* Directories / unknown size: follow FAT until EOC. */
    std::uint64_t left =
        whole_chain || file_size == 0
            ? std::numeric_limits<std::uint64_t>::max()
            : file_size;
    std::uint32_t c = start;
    std::set<std::uint32_t> seen;
    std::uint64_t cluster_bytes = static_cast<std::uint64_t>(g.spc) * g.bps;
    while (c >= 2 && c < g.clusters + 2 && !is_eoc(g.type, c) && left > 0) {
        if (!seen.insert(c).second)
            break;
        std::uint64_t take = std::min(left, cluster_bytes);
        /* For whole_chain dirs, rewrite full clusters even past logical EOF of last. */
        if (whole_chain)
            take = cluster_bytes;
        FsFileExtent e;
        e.offset = cluster_offset(g, c);
        e.length = take;
        if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
            exts.back().length += e.length;
        else
            exts.push_back(e);
        if (!whole_chain)
            left -= take;
        std::uint32_t next = fat_get(fat, g.type, c);
        if (is_eoc(g.type, next))
            break;
        c = next;
    }
    return exts;
}

std::string short_name(const unsigned char *dir) {
    char name[9], ext[4];
    std::memcpy(name, dir, 8);
    std::memcpy(ext, dir + 8, 3);
    name[8] = ext[3] = 0;
    for (int i = 7; i >= 0 && name[i] == ' '; --i)
        name[i] = 0;
    for (int i = 2; i >= 0 && ext[i] == ' '; --i)
        ext[i] = 0;
    std::string s = name;
    if (ext[0]) {
        s += '.';
        s += ext;
    }
    return s;
}

bool walk_dir(Device &dev, FatGeom &g, const std::vector<unsigned char> &fat, std::uint32_t start_clus,
              bool is_root_fat16, const std::string &prefix, FsScanResult &out) {
    std::vector<unsigned char> dirbuf;
    if (is_root_fat16 && (g.type == FsType::Fat12 || g.type == FsType::Fat16)) {
        std::uint64_t off = g.base + static_cast<std::uint64_t>(g.reserved + g.fats * g.fatsz) * g.bps;
        std::uint64_t len = static_cast<std::uint64_t>(g.root_secs) * g.bps;
        dirbuf.resize(static_cast<size_t>(len));
        std::string err;
        if (!dev.pread_all(dirbuf.data(), off, dirbuf.size(), &err)) {
            out.error = err;
            return false;
        }
    } else {
        auto exts = chain_extents(g, fat, start_clus, 0, true);
        for (const auto &e : exts) {
            std::vector<unsigned char> chunk(static_cast<size_t>(e.length));
            std::string err;
            if (!dev.pread_all(chunk.data(), e.offset, chunk.size(), &err)) {
                out.error = err;
                return false;
            }
            dirbuf.insert(dirbuf.end(), chunk.begin(), chunk.end());
        }
    }

    std::string lfn;
    for (size_t i = 0; i + 32 <= dirbuf.size(); i += 32) {
        const unsigned char *e = dirbuf.data() + i;
        if (e[0] == 0)
            break;
        if (e[0] == 0xE5)
            continue;
        if (e[11] == 0x0F) {
            /* LFN — assemble UCS-2 little endian loosely */
            char part[14];
            int n = 0;
            auto take = [&](int off) {
                std::uint16_t ch = r16(e + off);
                if (ch && ch < 0x80 && n < 13)
                    part[n++] = static_cast<char>(ch);
            };
            for (int k = 1; k <= 10; k += 2)
                take(k);
            for (int k = 14; k <= 25; k += 2)
                take(k);
            for (int k = 28; k <= 29; k += 2)
                take(k);
            part[n] = 0;
            lfn = std::string(part) + lfn;
            continue;
        }
        if (e[11] & 0x08) { /* volume label */
            lfn.clear();
            continue;
        }
        std::string name = lfn.empty() ? short_name(e) : lfn;
        lfn.clear();
        if (name == "." || name == "..")
            continue;
        std::string path = prefix + "/" + name;
        std::uint32_t clus = r16(e + 26);
        if (g.type == FsType::Fat32)
            clus |= static_cast<std::uint32_t>(r16(e + 20)) << 16;
        std::uint32_t fsize = r32(e + 28);
        bool is_dir = (e[11] & 0x10) != 0;

        FsObject obj;
        obj.path = path;
        obj.size = is_dir ? 0 : fsize;
        if (clus >= 2)
            obj.extents = chain_extents(g, fat, clus, is_dir ? 0 : fsize, is_dir);
        out.objects.push_back(obj);

        if (is_dir && clus >= 2) {
            if (!walk_dir(dev, g, fat, clus, false, path, out))
                return false;
        }
    }
    return true;
}

} /* namespace */

bool scan_fat(Device &dev, std::uint64_t base, FsType type, FsScanResult &out) {
    unsigned char bpb[512];
    std::string err;
    if (!dev.pread_all(bpb, base, sizeof bpb, &err)) {
        out.error = err;
        return false;
    }

    FatGeom g;
    g.type = type;
    g.base = base;
    g.bps = r16(bpb + 11);
    g.spc = bpb[13];
    g.reserved = r16(bpb + 14);
    g.fats = bpb[16];
    g.root_ents = r16(bpb + 17);
    std::uint16_t tot16 = r16(bpb + 19);
    std::uint16_t fatsz16 = r16(bpb + 22);
    std::uint32_t tot32 = r32(bpb + 32);
    std::uint32_t fatsz32 = r32(bpb + 36);
    g.fatsz = fatsz16 ? fatsz16 : fatsz32;
    g.totsec = tot16 ? tot16 : tot32;
    g.root_clus = (type == FsType::Fat32) ? r32(bpb + 44) : 0;
    g.root_secs = ((g.root_ents * 32) + (g.bps - 1)) / g.bps;
    g.fat_start = g.reserved;
    g.data_start = g.reserved + g.fats * g.fatsz + ((type == FsType::Fat32) ? 0 : g.root_secs);
    std::uint32_t data_secs = g.totsec > g.data_start ? g.totsec - g.data_start : 0;
    g.clusters = g.spc ? data_secs / g.spc : 0;

    if (!g.bps || !g.spc || !g.fats || !g.fatsz) {
        out.error = "invalid FAT BPB";
        return false;
    }

    /* Metadata objects */
    {
        FsObject boot;
        boot.path = "::boot";
        boot.size = g.reserved * g.bps;
        boot.extents.push_back({base, boot.size});
        out.objects.push_back(boot);

        FsObject fatobj;
        fatobj.path = "::fat";
        fatobj.size = static_cast<std::uint64_t>(g.fats) * g.fatsz * g.bps;
        fatobj.extents.push_back({base + static_cast<std::uint64_t>(g.fat_start) * g.bps, fatobj.size});
        out.objects.push_back(fatobj);
    }

    std::vector<unsigned char> fat(static_cast<size_t>(g.fatsz) * g.bps);
    if (!dev.pread_all(fat.data(), base + static_cast<std::uint64_t>(g.fat_start) * g.bps, fat.size(),
                       &err)) {
        out.error = err;
        return false;
    }

    if (type == FsType::Fat32)
        return walk_dir(dev, g, fat, g.root_clus, false, "", out);
    return walk_dir(dev, g, fat, 0, true, "", out);
}

} /* namespace reflash */
