/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal ext2/3/4 walker: superblock, inode tables, directory tree,
 * direct/indirect and extent-tree data blocks.
 */

#include "fs/ext.hpp"

#include <algorithm>
#include <cstring>
#include <map>
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

struct ExtGeom {
    std::uint64_t base = 0;
    std::uint32_t block_size = 1024;
    std::uint32_t inodes_per_group = 0;
    std::uint32_t blocks_per_group = 0;
    std::uint32_t inode_size = 128;
    std::uint32_t first_data_block = 1;
    std::uint32_t groups = 0;
    std::uint32_t inodes_count = 0;
    bool extents = false;
    std::vector<unsigned char> gd; /* group descriptors */
    std::uint32_t gd_entry_size = 32;
};

std::uint64_t blk_off(const ExtGeom &g, std::uint64_t block) {
    return g.base + block * g.block_size;
}

bool read_block(Device &dev, const ExtGeom &g, std::uint64_t block, std::vector<unsigned char> &out,
                std::string *err) {
    out.resize(g.block_size);
    return dev.pread_all(out.data(), blk_off(g, block), out.size(), err);
}

std::uint32_t inode_block_group(const ExtGeom &g, std::uint32_t ino) {
    return (ino - 1) / g.inodes_per_group;
}

std::uint32_t bg_inode_table(const ExtGeom &g, std::uint32_t group) {
    const unsigned char *e = g.gd.data() + group * g.gd_entry_size;
    if (g.gd_entry_size >= 40)
        return r32(e + 8); /* still lo 32 for classic layout; 64-bit would use hi */
    return r32(e + 8);
}

bool read_inode(Device &dev, const ExtGeom &g, std::uint32_t ino, std::vector<unsigned char> &inode,
                std::string *err) {
    if (ino == 0 || ino > g.inodes_count)
        return false;
    std::uint32_t group = inode_block_group(g, ino);
    std::uint32_t index = (ino - 1) % g.inodes_per_group;
    std::uint32_t table = bg_inode_table(g, group);
    std::uint64_t off = blk_off(g, table) + static_cast<std::uint64_t>(index) * g.inode_size;
    inode.resize(g.inode_size);
    return dev.pread_all(inode.data(), off, inode.size(), err);
}

void append_block_extents(std::vector<FsFileExtent> &exts, const ExtGeom &g, std::uint64_t block,
                          std::uint64_t bytes) {
    if (block == 0 || bytes == 0)
        return;
    FsFileExtent e{blk_off(g, block), bytes};
    if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
        exts.back().length += e.length;
    else
        exts.push_back(e);
}

bool parse_extents_full(Device &dev, const ExtGeom &g, const unsigned char *eh, std::uint64_t size,
                        std::vector<FsFileExtent> &exts, std::string *err) {
    exts.clear();
    std::uint64_t left = size;
    if (r16(eh) != 0xF30A)
        return false;
    std::uint16_t entries = r16(eh + 2);
    std::uint16_t depth = r16(eh + 6);
    if (depth == 0) {
        const unsigned char *p = eh + 12;
        for (std::uint16_t i = 0; i < entries && left > 0; ++i) {
            std::uint16_t len = r16(p + 4);
            bool uninit = (len & 0x8000) != 0;
            len = static_cast<std::uint16_t>(len & 0x7FFF);
            std::uint64_t start = static_cast<std::uint64_t>(r32(p + 8)) |
                                  (static_cast<std::uint64_t>(r16(p + 6)) << 32);
            p += 12;
            if (uninit) {
                std::uint64_t skip = static_cast<std::uint64_t>(len) * g.block_size;
                if (skip > left)
                    skip = left;
                left -= skip;
                continue;
            }
            for (std::uint16_t b = 0; b < len && left > 0; ++b) {
                std::uint64_t take = std::min(left, static_cast<std::uint64_t>(g.block_size));
                append_block_extents(exts, g, start + b, take);
                left -= take;
            }
        }
        return true;
    }
    const unsigned char *p = eh + 12;
    for (std::uint16_t i = 0; i < entries && left > 0; ++i) {
        std::uint64_t leaf = static_cast<std::uint64_t>(r32(p + 4)) |
                             (static_cast<std::uint64_t>(r16(p + 8)) << 32);
        p += 12;
        std::vector<unsigned char> blk;
        if (!read_block(dev, g, leaf, blk, err))
            return false;
        std::vector<FsFileExtent> part;
        std::uint64_t before = 0;
        for (const auto &e : exts)
            before += e.length;
        if (!parse_extents_full(dev, g, blk.data(), size - before, part, err))
            return false;
        for (const auto &e : part) {
            if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
                exts.back().length += e.length;
            else
                exts.push_back(e);
        }
        std::uint64_t used = 0;
        for (const auto &e : exts)
            used += e.length;
        left = size > used ? size - used : 0;
    }
    return true;
}

bool collect_indirect(Device &dev, const ExtGeom &g, std::uint32_t block, int level,
                      std::uint64_t &left, std::vector<FsFileExtent> &exts, std::string *err) {
    if (block == 0 || left == 0)
        return true;
    if (level == 0) {
        std::uint64_t take = std::min(left, static_cast<std::uint64_t>(g.block_size));
        append_block_extents(exts, g, block, take);
        left -= take;
        return true;
    }
    std::vector<unsigned char> blk;
    if (!read_block(dev, g, block, blk, err))
        return false;
    /* also rewrite indirect block itself */
    append_block_extents(exts, g, block, g.block_size);
    size_t n = g.block_size / 4;
    for (size_t i = 0; i < n && left > 0; ++i) {
        std::uint32_t b = r32(blk.data() + i * 4);
        if (!collect_indirect(dev, g, b, level - 1, left, exts, err))
            return false;
    }
    return true;
}

bool inode_data_extents(Device &dev, const ExtGeom &g, const unsigned char *inode,
                        std::vector<FsFileExtent> &exts, std::string *err) {
    std::uint16_t mode = r16(inode);
    std::uint64_t size = r32(inode + 4);
    if (g.inode_size >= 128 + 4)
        size |= static_cast<std::uint64_t>(r32(inode + 108)) << 32;
    std::uint32_t flags = r32(inode + 32);
    const std::uint32_t EXT4_EXTENTS_FL = 0x80000;

    if ((flags & EXT4_EXTENTS_FL) || g.extents) {
        return parse_extents_full(dev, g, inode + 40, size, exts, err);
    }

    std::uint64_t left = size;
    if ((mode & 0xF000) == 0x4000 && size == 0) {
        /* directory with unknown size — read at least direct blocks until empty */
        left = static_cast<std::uint64_t>(g.block_size) * 12;
    }
    for (int i = 0; i < 12 && left > 0; ++i) {
        std::uint32_t b = r32(inode + 40 + i * 4);
        if (!collect_indirect(dev, g, b, 0, left, exts, err))
            return false;
    }
    if (left > 0) {
        std::uint32_t b = r32(inode + 40 + 12 * 4);
        if (!collect_indirect(dev, g, b, 1, left, exts, err))
            return false;
    }
    if (left > 0) {
        std::uint32_t b = r32(inode + 40 + 13 * 4);
        if (!collect_indirect(dev, g, b, 2, left, exts, err))
            return false;
    }
    if (left > 0) {
        std::uint32_t b = r32(inode + 40 + 14 * 4);
        if (!collect_indirect(dev, g, b, 3, left, exts, err))
            return false;
    }
    return true;
}

bool walk_dir(Device &dev, ExtGeom &g, std::uint32_t ino, const std::string &prefix,
              FsScanResult &out, std::set<std::uint32_t> &seen) {
    if (!seen.insert(ino).second)
        return true;
    std::vector<unsigned char> inode;
    std::string err;
    if (!read_inode(dev, g, ino, inode, &err)) {
        out.error = err;
        return false;
    }
    std::uint16_t mode = r16(inode.data());
    std::uint64_t size = r32(inode.data() + 4);
    if (g.inode_size >= 128 + 4)
        size |= static_cast<std::uint64_t>(r32(inode.data() + 108)) << 32;

    std::vector<FsFileExtent> exts;
    if (!inode_data_extents(dev, g, inode.data(), exts, &err)) {
        out.error = err.empty() ? "inode extents failed" : err;
        return false;
    }

    if ((mode & 0xF000) == 0x8000) { /* regular file */
        FsObject obj;
        obj.path = prefix;
        obj.size = size;
        obj.extents = exts;
        out.objects.push_back(obj);
        return true;
    }
    if ((mode & 0xF000) != 0x4000)
        return true;

    /* directory — also rewrite directory blocks */
    {
        FsObject obj;
        obj.path = prefix.empty() ? "/" : prefix;
        obj.size = size;
        obj.extents = exts;
        out.objects.push_back(obj);
    }

    std::vector<unsigned char> data;
    for (const auto &e : exts) {
        std::vector<unsigned char> chunk(static_cast<size_t>(e.length));
        if (!dev.pread_all(chunk.data(), e.offset, chunk.size(), &err)) {
            out.error = err;
            return false;
        }
        data.insert(data.end(), chunk.begin(), chunk.end());
    }

    size_t pos = 0;
    while (pos + 8 <= data.size()) {
        std::uint32_t child = r32(data.data() + pos);
        std::uint16_t rec_len = r16(data.data() + pos + 4);
        std::uint8_t name_len = data[pos + 6];
        if (rec_len < 8 || pos + rec_len > data.size())
            break;
        if (child != 0 && name_len > 0) {
            std::string name(reinterpret_cast<char *>(data.data() + pos + 8), name_len);
            if (name != "." && name != "..") {
                std::string path = prefix + "/" + name;
                if (!walk_dir(dev, g, child, path, out, seen))
                    return false;
            }
        }
        pos += rec_len;
    }
    return true;
}

} /* namespace */

bool scan_ext(Device &dev, std::uint64_t base, FsType type, FsScanResult &out) {
    (void)type;
    unsigned char sb[1024];
    std::string err;
    if (!dev.pread_all(sb, base + 1024, sizeof sb, &err)) {
        out.error = err;
        return false;
    }
    if (r16(sb + 56) != 0xEF53) {
        out.error = "bad ext magic";
        return false;
    }

    ExtGeom g;
    g.base = base;
    std::uint32_t log_bs = r32(sb + 24);
    g.block_size = 1024u << log_bs;
    g.inodes_per_group = r32(sb + 40);
    g.blocks_per_group = r32(sb + 32);
    g.first_data_block = r32(sb + 20);
    g.inodes_count = r32(sb + 0);
    std::uint32_t blocks_count = r32(sb + 4);
    g.inode_size = r16(sb + 88);
    if (g.inode_size < 128)
        g.inode_size = 128;
    std::uint32_t feat_incompat = r32(sb + 96);
    const std::uint32_t INCOMPAT_EXTENTS = 0x40;
    const std::uint32_t INCOMPAT_64BIT = 0x80;
    g.extents = (feat_incompat & INCOMPAT_EXTENTS) != 0;
    g.gd_entry_size = (feat_incompat & INCOMPAT_64BIT) ? r16(sb + 254) : 32;
    if (g.gd_entry_size < 32)
        g.gd_entry_size = 32;

    g.groups = (blocks_count - g.first_data_block + g.blocks_per_group - 1) / g.blocks_per_group;
    std::uint64_t gd_block = g.first_data_block + 1;
    std::uint64_t gd_bytes = static_cast<std::uint64_t>(g.groups) * g.gd_entry_size;
    g.gd.resize(static_cast<size_t>(gd_bytes));
    if (!dev.pread_all(g.gd.data(), blk_off(g, gd_block), g.gd.size(), &err)) {
        out.error = err;
        return false;
    }

    /* metadata */
    {
        FsObject sbo;
        sbo.path = "::superblock";
        sbo.size = 1024;
        sbo.extents.push_back({base + 1024, 1024});
        out.objects.push_back(sbo);

        FsObject gdo;
        gdo.path = "::group_descriptors";
        gdo.size = gd_bytes;
        gdo.extents.push_back({blk_off(g, gd_block), gd_bytes});
        out.objects.push_back(gdo);

        for (std::uint32_t grp = 0; grp < g.groups; ++grp) {
            std::uint32_t table = bg_inode_table(g, grp);
            FsObject ito;
            ito.path = "::inode_table-" + std::to_string(grp);
            ito.size = static_cast<std::uint64_t>(g.inodes_per_group) * g.inode_size;
            ito.extents.push_back({blk_off(g, table), ito.size});
            out.objects.push_back(ito);
        }
    }

    std::set<std::uint32_t> seen;
    return walk_dir(dev, g, 2, "", out, seen);
}

} /* namespace reflash */
