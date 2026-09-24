/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>

namespace sdmsg {

class Device {
public:
    Device() = default;
    ~Device();

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    bool open(const std::string &path);
    void close();

    int fd() const { return fd_; }
    /* O_DIRECT companion fd when available; -1 if unsupported. */
    int fd_direct() const { return fd_direct_; }
    bool has_direct() const { return fd_direct_ >= 0; }
    bool is_block() const { return is_block_; }
    bool is_regular() const { return is_regular_; }
    std::uint64_t size() const { return size_; }
    std::uint64_t sector_size() const { return sector_size_; }
    const std::string &path() const { return path_; }

    /* Alignment required for O_DIRECT I/O (max of sector and page). */
    std::uint64_t direct_align() const { return direct_align_; }

    /* Read/write exact length; returns false on short I/O or error. */
    bool pread_all(void *buf, std::uint64_t offset, std::size_t len, std::string *err = nullptr);
    bool pwrite_all(const void *buf, std::uint64_t offset, std::size_t len,
                    std::string *err = nullptr);

    /* Bypass page cache when O_DIRECT is available. Buffer must be
     * direct_align()-aligned; offset/len should be multiples of the align. */
    bool pread_direct(void *buf, std::uint64_t offset, std::size_t len, std::string *err = nullptr);
    bool pwrite_direct(const void *buf, std::uint64_t offset, std::size_t len,
                       std::string *err = nullptr);

    /* Write then fdatasync (used when O_DIRECT is unavailable). */
    bool pwrite_sync(const void *buf, std::uint64_t offset, std::size_t len,
                     std::string *err = nullptr);

private:
    int fd_ = -1;
    int fd_direct_ = -1;
    bool is_block_ = false;
    bool is_regular_ = false;
    std::uint64_t size_ = 0;
    std::uint64_t sector_size_ = 512;
    std::uint64_t direct_align_ = 4096;
    std::string path_;

    static bool io_exact(int fd, bool writing, void *buf, std::uint64_t offset, std::size_t len,
                         std::string *err);
};

} /* namespace sdmsg */
