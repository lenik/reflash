/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "mount/automount.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sdmsg {

namespace {

std::string real_or_self(const std::string &path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf))
        return buf;
    return path;
}

bool same_device_file(const std::string &a, const std::string &b) {
    struct stat sa {}, sb {};
    if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_rdev == sb.st_rdev && sa.st_rdev != 0;
}

bool is_related_device(const std::string &wanted, const std::string &mounted_dev) {
    std::string w = real_or_self(wanted);
    std::string m = real_or_self(mounted_dev);
    if (w == m)
        return true;
    if (same_device_file(w, m))
        return true;
    if (m.size() > w.size() && m.compare(0, w.size(), w) == 0) {
        char c = m[w.size()];
        if (c == 'p' || (c >= '0' && c <= '9'))
            return true;
    }
    return false;
}

bool options_has_ro(const std::string &opts) {
    std::istringstream iss(opts);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        if (tok == "ro")
            return true;
    }
    return false;
}

int run_shell(const std::string &cmd) {
    int rc = system(cmd.c_str());
    if (rc < 0)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

std::string shell_quote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

bool try_privileged_mount(const std::string &device, const std::string &target,
                          const std::string &fstype, const std::string &options, std::string *err) {
    mkdir(target.c_str(), 0755);
    std::string opt = options.empty() ? "defaults" : options;
    std::string cmd = "mount -t " + shell_quote(fstype) + " -o " + shell_quote(opt) + " " +
                      shell_quote(device) + " " + shell_quote(target);
    if (run_shell(cmd) == 0)
        return true;
    if (run_shell("pkexec " + cmd) == 0)
        return true;
    if (run_shell("sudo " + cmd) == 0)
        return true;
    if (err)
        *err = "mount failed for " + device + " -> " + target + " (tried mount/pkexec/sudo)";
    return false;
}

bool try_privileged_umount(const std::string &target, std::string *err) {
    if (umount2(target.c_str(), 0) == 0)
        return true;
    if (umount2(target.c_str(), MNT_DETACH) == 0)
        return true;
    std::string cmd = "umount " + shell_quote(target);
    if (run_shell(cmd) == 0)
        return true;
    if (run_shell("pkexec " + cmd) == 0)
        return true;
    if (run_shell("sudo " + cmd) == 0)
        return true;
    if (err)
        *err = "umount failed: " + target;
    return false;
}

} /* namespace */

bool MountRecord::read_only() const { return options_has_ro(options); }

std::vector<MountRecord> find_mounts_for_device(const std::string &device_path) {
    std::vector<MountRecord> out;
    std::ifstream in("/proc/mounts");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        MountRecord r;
        if (!(iss >> r.device >> r.target >> r.fstype >> r.options))
            continue;
        if (is_related_device(device_path, r.device))
            out.push_back(r);
    }
    return out;
}

bool path_is_mounted(const std::string &device_path) {
    return !find_mounts_for_device(device_path).empty();
}

bool unmount_device(const std::string &device_path, std::string *err) {
    auto mounts = find_mounts_for_device(device_path);
    std::sort(mounts.begin(), mounts.end(),
              [](const MountRecord &a, const MountRecord &b) {
                  return a.target.size() > b.target.size();
              });
    for (const auto &m : mounts) {
        if (!try_privileged_umount(m.target, err))
            return false;
    }
    return true;
}

bool mount_device(const std::string &device, const std::string &target, const std::string &fstype,
                  const std::string &options, std::string *err) {
    unsigned long flags = 0;
    if (options_has_ro(options))
        flags |= MS_RDONLY;
    if (mount(device.c_str(), target.c_str(), fstype.c_str(), flags, nullptr) == 0)
        return true;
    return try_privileged_mount(device, target, fstype, options, err);
}

bool remount_records(const std::vector<MountRecord> &recs, std::string *err) {
    for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
        unsigned long flags = 0;
        if (it->read_only())
            flags |= MS_RDONLY;
        if (mount(it->device.c_str(), it->target.c_str(), it->fstype.c_str(), flags, nullptr) != 0) {
            if (!try_privileged_mount(it->device, it->target, it->fstype,
                                      it->read_only() ? "ro" : "rw", err))
                return false;
        }
    }
    return true;
}

bool ensure_unmounted(const std::string &device_path, bool auto_mount,
                      std::vector<MountRecord> *original, std::string *err) {
    auto mounts = find_mounts_for_device(device_path);
    if (original)
        *original = mounts;
    if (mounts.empty())
        return true;
    if (!auto_mount) {
        if (err)
            *err = "device is mounted; unmount first or use --auto-mount";
        return false;
    }
    return unmount_device(device_path, err);
}

Sha1MountChoice prompt_sha1_mount_choice(bool interactive) {
    const char *env = getenv("SDMSG_SHA1");
    if (env) {
        if (std::strcmp(env, "skip") == 0)
            return Sha1MountChoice::Skip;
        if (std::strcmp(env, "mount") == 0 || std::strcmp(env, "ro") == 0)
            return Sha1MountChoice::MountRo;
    }
    if (!interactive || !isatty(STDIN_FILENO)) {
        /* GUI / batch: attempt RO mount (pkexec may prompt); SDMSG_SHA1=skip to disable. */
        fprintf(stderr, "sdmsg: will try read-only mount for SHA-1 (SDMSG_SHA1=skip to disable)\n");
        return Sha1MountChoice::MountRo;
    }
    fprintf(stderr,
            "sdmsg: rewrite done; SHA-1 requires the filesystem mounted.\n"
            "  [s]kip SHA-1  or  [m]ount read-only (via sudo/pkexec if needed)? [s/m] ");
    fflush(stderr);
    char line[32];
    if (!fgets(line, sizeof line, stdin))
        return Sha1MountChoice::Skip;
    if (line[0] == 'm' || line[0] == 'M')
        return Sha1MountChoice::MountRo;
    return Sha1MountChoice::Skip;
}

bool prepare_sha1_mounts(const std::string &device_path, bool auto_mount, bool interactive,
                         const std::vector<MountRecord> &original,
                         std::vector<MountRecord> *active, bool *temp_created, std::string *err) {
    if (temp_created)
        *temp_created = false;
    auto cur = find_mounts_for_device(device_path);
    if (!cur.empty()) {
        *active = cur;
        return true;
    }

    if (auto_mount && !original.empty()) {
        /* Remount original targets read-only for hashing when possible. */
        std::vector<MountRecord> ro = original;
        for (auto &r : ro) {
            if (!options_has_ro(r.options)) {
                if (r.options.empty() || r.options == "rw")
                    r.options = "ro";
                else
                    r.options = "ro," + r.options;
            }
        }
        if (!remount_records(ro, err))
            return false;
        *active = find_mounts_for_device(device_path);
        return !active->empty();
    }

    auto choice = prompt_sha1_mount_choice(interactive);
    if (choice == Sha1MountChoice::Skip)
        return false; /* not an error — caller skips SHA-1 */

    /* Create temporary mount points under /tmp */
    if (original.empty()) {
        /* Whole device / single filesystem image — need fstype guess left to caller
         * via a temp mount of the device itself. */
        std::string mnt = "/tmp/sdmsg-mnt-XXXXXX";
        char tmpl[64];
        std::snprintf(tmpl, sizeof tmpl, "%s", mnt.c_str());
        if (!mkdtemp(tmpl)) {
            if (err)
                *err = "mkdtemp failed";
            return false;
        }
        /* Try common types */
        const char *types[] = {"auto", "vfat", "exfat", "ntfs3", "ntfs", "ext4", "ext3", "ext2",
                               nullptr};
        bool ok = false;
        for (int i = 0; types[i]; ++i) {
            std::string e;
            if (mount_device(device_path, tmpl, types[i], "ro", &e)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            rmdir(tmpl);
            if (err)
                *err = "could not mount " + device_path + " read-only";
            return false;
        }
        MountRecord r;
        r.device = device_path;
        r.target = tmpl;
        r.fstype = "auto";
        r.options = "ro";
        active->push_back(r);
        if (temp_created)
            *temp_created = true;
        return true;
    }

    for (const auto &o : original) {
        MountRecord r = o;
        r.options = "ro";
        if (!mount_device(r.device, r.target, r.fstype, "ro", err))
            return false;
        active->push_back(r);
    }
    if (temp_created)
        *temp_created = true;
    return true;
}

bool restore_mount_state(const std::string &device_path, const std::vector<MountRecord> &original,
                         const std::vector<MountRecord> &active, bool temp_created,
                         std::string *err) {
    (void)device_path;
    /* Tear down current mounts first */
    auto cur = active;
    if (cur.empty())
        cur = find_mounts_for_device(device_path);
    std::sort(cur.begin(), cur.end(),
              [](const MountRecord &a, const MountRecord &b) {
                  return a.target.size() > b.target.size();
              });
    for (const auto &m : cur) {
        try_privileged_umount(m.target, nullptr);
        if (temp_created && m.target.find("/tmp/sdmsg-mnt-") == 0)
            rmdir(m.target.c_str());
    }

    if (original.empty())
        return true; /* was originally unmounted */

    return remount_records(original, err);
}

} /* namespace sdmsg */
