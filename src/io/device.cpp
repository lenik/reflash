/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

namespace sdmsg {

Device::~Device() { close(); }

void Device::close() {
    if (fd_direct_ >= 0 && fd_direct_ != fd_) {
        ::close(fd_direct_);
        fd_direct_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    fd_direct_ = -1;
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

    long page = ::sysconf(_SC_PAGESIZE);
    if (page < 0)
        page = 4096;
    direct_align_ = sector_size_;
    if (static_cast<std::uint64_t>(page) > direct_align_)
        direct_align_ = static_cast<std::uint64_t>(page);

#if O_DIRECT != 0
    fd_direct_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_DIRECT);
    if (fd_direct_ < 0)
        fd_direct_ = -1;
#else
    fd_direct_ = -1;
#endif
    return true;
}

bool Device::io_exact(int fd, bool writing, void *buf, std::uint64_t offset, std::size_t len,
                      std::string *err) {
    if (fd < 0) {
        if (err)
            *err = "bad fd";
        return false;
    }
    auto *p = static_cast<unsigned char *>(buf);
    std::size_t left = len;
    while (left > 0) {
        ssize_t n;
        if (writing)
            n = ::pwrite(fd, p, left, static_cast<off_t>(offset));
        else
            n = ::pread(fd, p, left, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = strerror(errno);
            return false;
        }
        if (n == 0) {
            if (err)
                *err = writing ? "short write" : "unexpected EOF";
            return false;
        }
        p += n;
        offset += static_cast<std::uint64_t>(n);
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

bool Device::pread_all(void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    return io_exact(fd_, false, buf, offset, len, err);
}

bool Device::pwrite_all(const void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    return io_exact(fd_, true, const_cast<void *>(buf), offset, len, err);
}

bool Device::pread_direct(void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    if (fd_direct_ < 0) {
        if (err)
            *err = "O_DIRECT unavailable";
        return false;
    }
    return io_exact(fd_direct_, false, buf, offset, len, err);
}

bool Device::pwrite_direct(const void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    if (fd_direct_ < 0) {
        if (err)
            *err = "O_DIRECT unavailable";
        return false;
    }
    return io_exact(fd_direct_, true, const_cast<void *>(buf), offset, len, err);
}

bool Device::pwrite_sync(const void *buf, std::uint64_t offset, std::size_t len, std::string *err) {
    if (!pwrite_all(buf, offset, len, err))
        return false;
    if (::fdatasync(fd_) != 0) {
        if (err)
            *err = strerror(errno);
        return false;
    }
    return true;
}

} /* namespace sdmsg */
