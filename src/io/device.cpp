/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "io/device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fs.h>
#endif

namespace sdmsg {

Device::~Device() { close(); }

void Device::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Device::open(const std::string &path) {
    close();
    path_ = path;
    /* Never O_TRUNC — regular files must not be shortened. */
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        close();
        return false;
    }

    is_block_ = S_ISBLK(st.st_mode);
    is_regular_ = S_ISREG(st.st_mode);
    if (!is_block_ && !is_regular_) {
        close();
        return false;
    }

    sector_size_ = 512;
#ifdef __linux__
    if (is_block_) {
        unsigned int ssz = 0;
        if (ioctl(fd_, BLKSSZGET, &ssz) == 0 && ssz > 0)
            sector_size_ = ssz;
        unsigned long long bytes = 0;
        if (ioctl(fd_, BLKGETSIZE64, &bytes) == 0)
            size_ = bytes;
        else {
            close();
            return false;
        }
    } else
#endif
    {
        off_t end = lseek(fd_, 0, SEEK_END);
        if (end < 0) {
            close();
            return false;
        }
        size_ = static_cast<std::uint64_t>(end);
        if (st.st_blksize > 0)
            sector_size_ = static_cast<std::uint64_t>(st.st_blksize);
        lseek(fd_, 0, SEEK_SET);
    }
    return true;
}

bool Device::pread_all(void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    auto *p = static_cast<unsigned char *>(buf);
    std::size_t left = len;
    while (left > 0) {
        ssize_t n = ::pread(fd_, p, left, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = strerror(errno);
            return false;
        }
        if (n == 0) {
            if (err)
                *err = "unexpected EOF";
            return false;
        }
        p += n;
        offset += static_cast<std::uint64_t>(n);
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

bool Device::pwrite_all(const void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    auto *p = static_cast<const unsigned char *>(buf);
    std::size_t left = len;
    while (left > 0) {
        ssize_t n = ::pwrite(fd_, p, left, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = strerror(errno);
            return false;
        }
        if (n == 0) {
            if (err)
                *err = "short write";
            return false;
        }
        p += n;
        offset += static_cast<std::uint64_t>(n);
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

} /* namespace sdmsg */
