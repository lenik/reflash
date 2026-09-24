/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * NTFS walker: boot + $MFT and all allocated non-resident attribute runs.
 * Compressed/encrypted/sparse: rewrite allocated clusters as stored; skip holes.
 */

#include "fs/ntfs.hpp"

#include <cstring>
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

std::int64_t read_signed(const unsigned char *p, int n) {
    std::int64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= static_cast<std::int64_t>(p[i]) << (8 * i);
    if (n > 0 && (p[n - 1] & 0x80)) {
        for (int i = n; i < 8; ++i)
            v |= static_cast<std::int64_t>(0xFF) << (8 * i);
    }
    return v;
}

/* Follow the entire runlist; skip sparse (unallocated) holes. */
std::vector<FsFileExtent> decode_runs_allocated(const unsigned char *run, std::uint64_t base,
                                                std::uint32_t bps, std::uint32_t spc) {
    std::vector<FsFileExtent> exts;
    std::int64_t lcn = 0;
    std::uint64_t cluster_bytes = static_cast<std::uint64_t>(bps) * spc;
    while (*run) {
        unsigned char header = *run++;
        int len_len = header & 0x0F;
        int off_len = (header >> 4) & 0x0F;
        if (len_len == 0)
            break;
        std::uint64_t run_len = 0;
        for (int i = 0; i < len_len; ++i)
            run_len |= static_cast<std::uint64_t>(run[i]) << (8 * i);
        run += len_len;
        std::int64_t off = 0;
        if (off_len) {
            off = read_signed(run, off_len);
            run += off_len;
            lcn += off;
        }
        if (off_len == 0) {
            /* sparse / unallocated hole — do not massage */
            continue;
        }
        std::uint64_t bytes = run_len * cluster_bytes;
        FsFileExtent e;
        e.offset = base + static_cast<std::uint64_t>(lcn) * cluster_bytes;
        e.length = bytes;
        if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
            exts.back().length += e.length;
        else
            exts.push_back(e);
    }
    return exts;
}

bool fixup_record(std::vector<unsigned char> &rec, std::uint16_t sector_size) {
    if (rec.size() < 8 || std::memcmp(rec.data(), "FILE", 4) != 0)
        return false;
    std::uint16_t usa_off = r16(rec.data() + 4);
    std::uint16_t usa_count = r16(rec.data() + 6);
    if (usa_off == 0 || usa_count < 2)
        return true;
    if (static_cast<size_t>(usa_off) + usa_count * 2 > rec.size())
        return false;
    const unsigned char *usa = rec.data() + usa_off;
    for (std::uint16_t i = 1; i < usa_count; ++i) {
        size_t pos = static_cast<size_t>(i) * sector_size - 2;
        if (pos + 2 > rec.size())
            return false;
        rec[pos] = usa[i * 2];
        rec[pos + 1] = usa[i * 2 + 1];
    }
    return true;
}

void collect_nonresident_attrs(const std::vector<unsigned char> &rec, std::uint64_t base,
                               std::uint16_t bps, std::uint8_t spc,
                               std::vector<FsFileExtent> &exts, std::uint64_t &logical_size,
                               std::string *name_out) {
    std::uint16_t aoff = r16(rec.data() + 20);
    while (aoff + 8 < rec.size()) {
        const unsigned char *a = rec.data() + aoff;
        std::uint32_t atype = r32(a);
        std::uint32_t alen = r32(a + 4);
        if (atype == 0xFFFFFFFF || alen < 8)
            break;
        if (atype == 0x30 && a[8] == 0 && name_out) {
            std::uint16_t content_off = r16(a + 20);
            if (static_cast<size_t>(aoff) + content_off + 66 < rec.size()) {
                const unsigned char *c = a + content_off;
                unsigned char nlen = c[64];
                unsigned char nspace = c[65];
                if (nspace <= 3 && nlen > 0) {
                    name_out->clear();
                    for (unsigned i = 0; i < nlen; ++i) {
                        std::uint16_t ch = r16(c + 66 + i * 2);
                        if (ch < 0x80)
                            name_out->push_back(static_cast<char>(ch));
                        else
                            name_out->push_back('?');
                    }
                }
            }
        }
        /* Any non-resident attribute: rewrite allocated clusters as stored
         * (compressed / encrypted ciphertext included). */
        if (a[8] != 0 && alen >= 64) {
            std::uint16_t run_off = r16(a + 32);
            if (run_off > 0 && static_cast<size_t>(run_off) < alen) {
                auto part = decode_runs_allocated(a + run_off, base, bps, spc);
                for (const auto &e : part) {
                    if (!exts.empty() && exts.back().offset + exts.back().length == e.offset)
                        exts.back().length += e.length;
                    else
                        exts.push_back(e);
                }
            }
            if (atype == 0x80)
                logical_size = r64(a + 48);
        } else if (atype == 0x80 && a[8] == 0) {
            logical_size = r32(a + 16);
        }
        aoff = static_cast<std::uint16_t>(aoff + alen);
    }
}

} /* namespace */

bool scan_ntfs(Device &dev, std::uint64_t base, FsScanResult &out) {
    unsigned char boot[512];
    std::string err;
    if (!dev.pread_all(boot, base, sizeof boot, &err)) {
        out.error = err;
        return false;
    }
    if (std::memcmp(boot + 3, "NTFS    ", 8) != 0) {
        out.error = "not NTFS";
        return false;
    }

    std::uint16_t bps = r16(boot + 11);
    std::uint8_t spc = boot[13];
    std::uint64_t mft_lcn = r64(boot + 48);
    std::int8_t clusters_per_mft = static_cast<std::int8_t>(boot[64]);
    std::uint32_t mft_record_size;
    if (clusters_per_mft > 0)
        mft_record_size = static_cast<std::uint32_t>(clusters_per_mft) * bps * spc;
    else
        mft_record_size = 1u << static_cast<unsigned>(-clusters_per_mft);

    if (!bps || !spc || mft_record_size < 1024) {
        out.error = "invalid NTFS boot";
        return false;
    }

    FsObject boot_obj;
    boot_obj.path = "::boot";
    boot_obj.size = bps;
    boot_obj.extents.push_back({base, bps});
    out.objects.push_back(boot_obj);

    std::uint64_t cluster_bytes = static_cast<std::uint64_t>(bps) * spc;
    std::uint64_t mft_off = base + mft_lcn * cluster_bytes;

    std::vector<unsigned char> mft0(mft_record_size);
    if (!dev.pread_all(mft0.data(), mft_off, mft0.size(), &err)) {
        out.error = err;
        return false;
    }
    if (!fixup_record(mft0, bps)) {
        out.error = "bad $MFT record";
        return false;
    }

    std::vector<FsFileExtent> mft_exts;
    std::uint64_t mft_logical = 0;
    collect_nonresident_attrs(mft0, base, bps, spc, mft_exts, mft_logical, nullptr);

    if (mft_exts.empty()) {
        FsObject mft;
        mft.path = "::$MFT";
        mft.size = mft_record_size;
        mft.extents.push_back({mft_off, mft_record_size});
        out.objects.push_back(mft);
        return true;
    }

    {
        FsObject mft;
        mft.path = "::$MFT";
        for (const auto &e : mft_exts)
            mft.size += e.length;
        mft.extents = mft_exts;
        out.objects.push_back(mft);
    }

    std::vector<unsigned char> mft_data;
    for (const auto &e : mft_exts) {
        std::vector<unsigned char> chunk(static_cast<size_t>(e.length));
        if (!dev.pread_all(chunk.data(), e.offset, chunk.size(), &err)) {
            out.error = err;
            return false;
        }
        mft_data.insert(mft_data.end(), chunk.begin(), chunk.end());
    }

    for (size_t off = 0; off + mft_record_size <= mft_data.size(); off += mft_record_size) {
        std::vector<unsigned char> rec(mft_data.begin() + static_cast<std::ptrdiff_t>(off),
                                       mft_data.begin() + static_cast<std::ptrdiff_t>(off + mft_record_size));
        if (!fixup_record(rec, bps))
            continue;
        if (std::memcmp(rec.data(), "FILE", 4) != 0)
            continue;
        std::uint16_t flags = r16(rec.data() + 22);
        if (!(flags & 0x01))
            continue;

        std::string name = "file-" + std::to_string(off / mft_record_size);
        std::vector<FsFileExtent> data_exts;
        std::uint64_t data_size = 0;
        collect_nonresident_attrs(rec, base, bps, spc, data_exts, data_size, &name);

        if (data_exts.empty() && data_size == 0)
            continue;

        /* Skip $MFT itself (already added) */
        if (off == 0)
            continue;

        FsObject obj;
        obj.path = "/" + name;
        obj.size = data_size;
        obj.extents = data_exts;
        out.objects.push_back(obj);
    }
    return true;
}

} /* namespace reflash */
