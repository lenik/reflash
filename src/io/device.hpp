/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    bool is_block() const { return is_block_; }
    bool is_regular() const { return is_regular_; }
    std::uint64_t size() const { return size_; }
    std::uint64_t sector_size() const { return sector_size_; }
    const std::string &path() const { return path_; }

    /* Read/write exact length; returns false on short I/O or error. */
    bool pread_all(void *buf, std::uint64_t offset, std::size_t len, std::string *err = nullptr);
    bool pwrite_all(const void *buf, std::uint64_t offset, std::size_t len,
                    std::string *err = nullptr);

private:
    int fd_ = -1;
    bool is_block_ = false;
    bool is_regular_ = false;
    std::uint64_t size_ = 0;
    std::uint64_t sector_size_ = 512;
    std::string path_;
};

} /* namespace sdmsg */
