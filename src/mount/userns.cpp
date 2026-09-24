/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mount/userns.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace reflash {

namespace {

bool g_in_user_mount_ns = false;

bool write_proc_file(const char *path, const std::string &data, std::string *err) {
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err)
            *err = std::string(path) + ": " + std::strerror(errno);
        return false;
    }
    ssize_t n = ::write(fd, data.data(), data.size());
    int saved = errno;
    ::close(fd);
    if (n < 0 || static_cast<size_t>(n) != data.size()) {
        if (err)
            *err = std::string(path) + ": " + std::strerror(saved);
        return false;
    }
    return true;
}

bool userns_allowed() {
    std::ifstream in("/proc/sys/kernel/unprivileged_userns_clone");
    if (!in)
        return true; /* older kernels without the sysctl — try anyway */
    int v = 1;
    in >> v;
    return v != 0;
}

} /* namespace */

bool in_user_mount_ns() { return g_in_user_mount_ns; }

bool ensure_user_mount_ns(std::string *err) {
    if (g_in_user_mount_ns)
        return true;

    if (!userns_allowed()) {
        if (err)
            *err = "unprivileged user namespaces disabled "
                   "(kernel.unprivileged_userns_clone=0)";
        return false;
    }

    /* Detect multi-thread (unshare NEWUSER fails with EINVAL). */
    {
        int threads = 0;
        std::ifstream st("/proc/self/status");
        std::string key;
        while (st >> key) {
            if (key == "Threads:") {
                st >> threads;
                break;
            }
            st.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
        if (threads > 1) {
            if (err)
                *err = "unshare(CLONE_NEWUSER): process is multi-threaded "
                       "(enter user mount namespace before starting threads)";
            return false;
        }
    }

    uid_t uid = ::geteuid();
    gid_t gid = ::getegid();

    /* NEWUSER first so we can claim CAP_SYS_ADMIN for NEWNS. */
    if (::unshare(CLONE_NEWUSER) != 0) {
        if (err) {
            *err = std::string("unshare(CLONE_NEWUSER): ") + std::strerror(errno);
            if (errno == EINVAL)
                *err += " (often: already in a user ns, or process is multi-threaded)";
        }
        return false;
    }

    /* Deny setgroups before writing gid_map (required for unprivileged). */
    if (!write_proc_file("/proc/self/setgroups", "deny\n", err))
        return false;

    {
        std::string map = "0 " + std::to_string(uid) + " 1\n";
        if (!write_proc_file("/proc/self/uid_map", map, err))
            return false;
    }
    {
        std::string map = "0 " + std::to_string(gid) + " 1\n";
        if (!write_proc_file("/proc/self/gid_map", map, err))
            return false;
    }

    if (::unshare(CLONE_NEWNS) != 0) {
        if (err)
            *err = std::string("unshare(CLONE_NEWNS): ") + std::strerror(errno);
        return false;
    }

    /* Avoid propagating mounts back to the host mount namespace. */
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        /* Non-fatal on some setups; continue. */
    }

    /*
     * Inside a user namespace, GIO's GVFS daemon peer-to-peer D-Bus auth
     * (EXTERNAL) fails against host gvfsd and spam GVFS-WARNINGs. Force the
     * local VFS before any GTK/GIO init so MIME/icons use GLocalVfs instead.
     */
    ::setenv("GIO_USE_VFS", "local", 1);

    g_in_user_mount_ns = true;
    return true;
}

} /* namespace reflash */
