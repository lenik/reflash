/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "mount/automount.hpp"
#include "fs/detect.hpp"
#include "io/device.hpp"
#include "io/partition.hpp"
#include "mount/host_priv.hpp"
#include "mount/userns.hpp"

#include <algorithm>
#include <cctype>
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

namespace reflash {

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

std::string run_capture(const std::string &cmd) {
    std::string out;
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp)
        return out;
    char buf[4096];
    while (fgets(buf, sizeof buf, fp))
        out += buf;
    pclose(fp);
    return out;
}

bool path_is_regular_file(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

bool which_ok(const char *bin) {
    std::string cmd = "command -v " + shell_quote(bin) + " >/dev/null 2>&1";
    return run_shell(cmd) == 0;
}

std::string parse_mount_point(const std::string &text) {
    /* "Mounted /dev/... at /run/media/..." */
    auto at = text.find(" at ");
    if (at == std::string::npos)
        return {};
    at += 4;
    size_t end = at;
    while (end < text.size() && text[end] != '\n' && text[end] != '.' && text[end] != ' ')
        ++end;
    return text.substr(at, end - at);
}

bool try_udisks_mount_block(const std::string &block, const std::string &options, MountRecord *out,
                            std::string *err) {
    if (!which_ok("udisksctl"))
        return false;
    std::string opt = options.empty() ? "ro" : options;
    std::string cmd = "udisksctl mount -b " + shell_quote(block) + " -o " + shell_quote(opt) +
                      " --no-user-interaction 2>&1";
    std::string text = run_capture(cmd);
    std::string mnt = parse_mount_point(text);
    if (mnt.empty()) {
        /* Fallback: see if already mounted */
        auto mounts = find_mounts_for_device(block);
        if (!mounts.empty()) {
            *out = mounts.front();
            return true;
        }
        if (err)
            *err = "udisksctl mount failed: " + text;
        return false;
    }
    out->device = block;
    out->target = mnt;
    out->fstype = "auto";
    out->options = opt;
    return true;
}

bool try_fuse_mount_file(const std::string &image, const std::string &target,
                         const std::string &fstype, const std::string &options,
                         std::uint64_t offset, MountRecord *rec, std::string *err) {
    mkdir(target.c_str(), 0755);
    std::string opt = options.empty() ? "ro" : options;
    if (offset > 0)
        opt += ",offset=" + std::to_string(offset);
    if (opt.find("fakeroot") == std::string::npos &&
        (fstype.find("ext") != std::string::npos || fstype == "auto"))
        opt += ",fakeroot";
    struct Candidate {
        const char *bin;
        const char *match; /* empty = always try; else match fstype */
    };
    const Candidate cands[] = {
        {"fuse2fs", "ext"},
        {"fuse2fs", "auto"},
        {"ntfs-3g", "ntfs"},
        {"ntfs-3g", "auto"},
        {"fuse.exfat", "exfat"},
        {"fuse.exfat", "auto"},
        {"exfat-fuse", "exfat"},
        {"exfat-fuse", "auto"},
        {"fusefat", "vfat"},
        {"fusefat", "fat"},
        {"fusefat", "auto"},
        {nullptr, nullptr},
    };
    auto mounted_here = [&]() -> bool {
        /* fuse2fs can exit 0 even when fusermount fails — check /proc/mounts. */
        std::ifstream in("/proc/mounts");
        std::string line;
        std::string want = real_or_self(target);
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string dev, mnt;
            if (!(iss >> dev >> mnt))
                continue;
            if (mnt == target || mnt == want)
                return true;
        }
        return run_shell("mountpoint -q " + shell_quote(target) + " 2>/dev/null") == 0;
    };

    auto try_cands = [&](std::string *out_err) -> bool {
        std::string last_err;
        std::string tried;
        for (int i = 0; cands[i].bin; ++i) {
            if (cands[i].match[0] && fstype != "auto" &&
                fstype.find(cands[i].match) == std::string::npos)
                continue;
            if (!which_ok(cands[i].bin))
                continue;
            if (!tried.empty())
                tried += ", ";
            tried += cands[i].bin;
            std::string diag =
                run_capture(std::string(cands[i].bin) + " -o " + shell_quote(opt) + " " +
                            shell_quote(image) + " " + shell_quote(target) + " 2>&1");
            if (mounted_here()) {
                if (rec) {
                    rec->device = image;
                    rec->target = target;
                    rec->fstype = fstype.empty() ? "fuse" : fstype;
                    rec->options = opt;
                    rec->fuse_mount = true;
                }
                return true;
            }
            if (last_err.empty()) {
                while (!diag.empty() && (diag.back() == '\n' || diag.back() == '\r'))
                    diag.pop_back();
                for (char &c : diag) {
                    if (c == '\n')
                        c = ' ';
                }
                last_err = std::string(cands[i].bin) + " did not mount " + image;
                if (!diag.empty())
                    last_err += ": " + diag;
            }
        }
        if (out_err) {
            if (last_err.empty())
                *out_err = "no FUSE helper could mount " + image;
            else if (fstype == "auto" && tried.find(',') != std::string::npos)
                *out_err = last_err + " (tried " + tried + ")";
            else
                *out_err = last_err;
        }
        return false;
    };

    std::string first_err;
    if (try_cands(&first_err))
        return true;

    /* Host mount often fails with EPERM when fusermount is not setuid.
     * Enter a user+mount namespace (CAP_SYS_ADMIN in-ns) and retry once. */
    bool looks_perm = first_err.find("Operation not permitted") != std::string::npos ||
                      first_err.find("EPERM") != std::string::npos ||
                      first_err.find("Permission denied") != std::string::npos;
    if (!in_user_mount_ns() && looks_perm) {
        std::string ns_err;
        if (ensure_user_mount_ns(&ns_err)) {
            std::string second_err;
            if (try_cands(&second_err))
                return true;
            if (err)
                *err = second_err.empty() ? first_err
                                          : (second_err + " [after user mount namespace]");
            return false;
        }
        if (err)
            *err = first_err + "; userns retry failed: " + ns_err;
        return false;
    }

    if (err)
        *err = first_err;
    return false;
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
    if (!host_priv_gui() && run_shell("sudo " + cmd) == 0)
        return true;
    if (err)
        *err = "mount failed for " + device + " -> " + target +
               (host_priv_gui() ? " (tried mount/pkexec)" : " (tried mount/pkexec/sudo)");
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
    if (!host_priv_gui() && run_shell("sudo " + cmd) == 0)
        return true;
    if (err)
        *err = "umount failed: " + target;
    return false;
}

bool unmount_one(const MountRecord &m, std::string *err) {
    const bool quiet = m.fuse_mount || no_auth_target(m.device) ||
                       (!m.loop_from_image.empty() && no_auth_target(m.loop_from_image));
    if (m.fuse_mount || quiet) {
        if (run_shell("fusermount -u " + shell_quote(m.target) + " 2>/dev/null") == 0)
            return true;
        if (run_shell("fusermount3 -u " + shell_quote(m.target) + " 2>/dev/null") == 0)
            return true;
        if (umount2(m.target.c_str(), 0) == 0 || umount2(m.target.c_str(), MNT_DETACH) == 0)
            return true;
        if (run_shell("umount " + shell_quote(m.target) + " 2>/dev/null") == 0)
            return true;
        if (quiet) {
            if (err)
                *err = "umount failed without privileges: " + m.target;
            return false;
        }
    }
    if (!m.loop_from_image.empty() && which_ok("udisksctl") && !quiet) {
        run_shell("udisksctl unmount -b " + shell_quote(m.device) +
                  " --no-user-interaction >/dev/null 2>&1");
        run_shell("udisksctl loop-delete -b " + shell_quote(m.device) +
                  " --no-user-interaction >/dev/null 2>&1");
        return true;
    }
    if (!quiet && which_ok("udisksctl") && path_is_block(m.device)) {
        if (run_shell("udisksctl unmount -b " + shell_quote(m.device) +
                      " --no-user-interaction >/dev/null 2>&1") == 0)
            return true;
    }
    return try_privileged_umount(m.target, err);
}

} /* namespace */

bool path_is_block(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISBLK(st.st_mode);
}

bool path_is_directory(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

bool no_auth_target(const std::string &path) {
    /* Regular files and loop devices are mounted with FUSE. udisksctl/pkexec
     * open a polkit dialog and are not used for these. */
    if (path_is_regular_file(path))
        return true;
    std::string p = real_or_self(path);
    return p.compare(0, 9, "/dev/loop") == 0;
}

bool MountRecord::read_only() const { return options_has_ro(options); }

std::vector<MountRecord> find_mounts_for_device(const std::string &device_path) {
    std::vector<MountRecord> out;
    std::ifstream in("/proc/mounts");
    std::string line;
    std::string want = real_or_self(device_path);
    std::vector<std::string> also; /* loop devices backing a regular-file image */
    if (path_is_regular_file(device_path)) {
        std::string ls = run_capture("losetup -j " + shell_quote(want) + " 2>/dev/null");
        std::istringstream iss(ls);
        std::string l;
        while (std::getline(iss, l)) {
            auto colon = l.find(':');
            if (colon != std::string::npos && l.compare(0, 5, "/dev/") == 0)
                also.push_back(l.substr(0, colon));
        }
    }
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        MountRecord r;
        if (!(iss >> r.device >> r.target >> r.fstype >> r.options))
            continue;
        if (is_related_device(device_path, r.device)) {
            out.push_back(r);
            continue;
        }
        if (r.device == want) {
            out.push_back(r);
            continue;
        }
        for (const auto &lp : also) {
            if (r.device == lp || is_related_device(lp, r.device)) {
                r.loop_from_image = want;
                out.push_back(r);
                break;
            }
        }
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
        if (!unmount_one(m, err))
            return false;
    }
    return true;
}

std::string trim_ws(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

/* Whole-disk node for udisks power-off (partition → parent disk). */
std::string udisks_drive_node(const std::string &device_path) {
    if (!path_is_block(device_path))
        return {};
    std::string path = real_or_self(device_path);
    std::string pk = trim_ws(run_capture("lsblk -no PKNAME " + shell_quote(path) + " 2>/dev/null"));
    if (!pk.empty())
        return "/dev/" + pk;
    return path;
}

bool eject_device(const std::string &device_path, std::string *err) {
    if (device_path.empty()) {
        if (err)
            *err = "no device";
        return false;
    }
    /* Directory targets: nothing to unmount at the block layer. */
    if (path_is_directory(device_path))
        return true;

    if (!unmount_device(device_path, err))
        return false;

    /* Detach loop for image files after FUSE/loop unmount. */
    if (path_is_regular_file(device_path)) {
        std::string want = real_or_self(device_path);
        std::string ls = run_capture("losetup -j " + shell_quote(want) + " 2>/dev/null");
        std::istringstream iss(ls);
        std::string line;
        while (std::getline(iss, line)) {
            auto colon = line.find(':');
            if (colon != std::string::npos && line.compare(0, 5, "/dev/") == 0) {
                std::string loop = line.substr(0, colon);
                run_shell("losetup -d " + shell_quote(loop) + " 2>/dev/null");
                if (which_ok("udisksctl"))
                    run_shell("udisksctl loop-delete -b " + shell_quote(loop) +
                              " --no-user-interaction >/dev/null 2>&1");
            }
        }
        return true;
    }

    if (!path_is_block(device_path))
        return true;

    std::string drive = udisks_drive_node(device_path);
    std::string type = trim_ws(run_capture("lsblk -no TYPE " + shell_quote(device_path) +
                                           " 2>/dev/null"));
    if (type == "rom" || type == "cd/dvd") {
        if (which_ok("eject"))
            run_shell("eject " + shell_quote(device_path) + " 2>/dev/null");
        else if (which_ok("udisksctl"))
            run_shell("udisksctl unmount -b " + shell_quote(device_path) +
                      " --no-user-interaction >/dev/null 2>&1");
    }
    (void)drive;
    return true;
}

bool safely_remove_device(const std::string &device_path, std::string *err) {
    if (device_path.empty()) {
        if (err)
            *err = "no device";
        return false;
    }
    if (path_is_directory(device_path))
        return true;

    if (!eject_device(device_path, err))
        return false;

    /* Image/loop: unmount (+ loop detach) is enough. */
    if (!path_is_block(device_path))
        return true;

    run_shell("sync");
    std::string drive = udisks_drive_node(device_path);
    if (drive.empty())
        drive = real_or_self(device_path);

    if (which_ok("udisksctl")) {
        std::string out =
            run_capture("udisksctl power-off -b " + shell_quote(drive) +
                        " --no-user-interaction 2>&1");
        if (out.find("Error") != std::string::npos || out.find("error") != std::string::npos) {
            /* Some devices reject power-off after unmount; treat as soft failure
             * if already unmounted — user can still unplug. */
            if (path_is_mounted(device_path)) {
                if (err)
                    *err = out.empty() ? ("power-off failed: " + drive) : trim_ws(out);
                return false;
            }
        }
        return true;
    }

    /* No udisks: unmount already done. */
    return true;
}

bool mount_device(const std::string &device, const std::string &target, const std::string &fstype,
                  const std::string &options, std::string *err) {
    unsigned long flags = 0;
    if (options_has_ro(options))
        flags |= MS_RDONLY;

    /* Files and loop devices: FUSE only. No udisksctl/pkexec (those pop a login). */
    if (no_auth_target(device)) {
        MountRecord rec;
        std::string e;
        if (try_fuse_mount_file(device, target, fstype.empty() ? "auto" : fstype,
                                options.empty() ? "ro" : options, 0, &rec, &e))
            return true;
        if (err)
            *err = e.empty() ? "FUSE mount failed for " + device : e;
        return false;
    }

    if (path_is_block(device) && which_ok("udisksctl")) {
        MountRecord rec;
        if (try_udisks_mount_block(device, options.empty() ? "ro" : options, &rec, err))
            return true;
    }

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
            if (no_auth_target(it->device)) {
                MountRecord rec;
                if (!try_fuse_mount_file(it->device, it->target, it->fstype,
                                         it->read_only() ? "ro" : "rw", 0, &rec, err))
                    return false;
            } else if (!try_privileged_mount(it->device, it->target, it->fstype,
                                             it->read_only() ? "ro" : "rw", err)) {
                return false;
            }
        }
    }
    return true;
}

bool ensure_unmounted(const std::string &device_path, std::vector<MountRecord> *original,
                      std::string *err) {
    auto mounts = find_mounts_for_device(device_path);
    if (original)
        *original = mounts;
    if (mounts.empty())
        return true;
    return unmount_device(device_path, err);
}

bool prepare_sha1_mounts(const std::string &device_path, const std::vector<MountRecord> &original,
                         std::vector<MountRecord> *active, bool *temp_created, std::string *err) {
    if (temp_created)
        *temp_created = false;
    auto cur = find_mounts_for_device(device_path);
    if (!cur.empty()) {
        *active = cur;
        return true;
    }

    if (!original.empty()) {
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

    /* Regular file or loop: detect FS and FUSE-mount like Browser (never polkit). */
    if (no_auth_target(device_path)) {
        return mount_for_browse(device_path, active, temp_created, err);
    }

    if (original.empty()) {
        std::string mnt = "/tmp/reflash-mnt-XXXXXX";
        char tmpl[64];
        std::snprintf(tmpl, sizeof tmpl, "%s", mnt.c_str());
        if (!mkdtemp(tmpl)) {
            if (err)
                *err = "mkdtemp failed";
            return false;
        }
        if (path_is_block(device_path) && !no_auth_target(device_path)) {
            MountRecord rec;
            if (try_udisks_mount_block(device_path, "ro", &rec, err)) {
                active->push_back(rec);
                rmdir(tmpl);
                if (temp_created)
                    *temp_created = true;
                return true;
            }
        }
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
        auto found = find_mounts_for_device(device_path);
        if (!found.empty()) {
            *active = found;
        } else {
            MountRecord r;
            r.device = device_path;
            r.target = tmpl;
            r.fstype = "auto";
            r.options = "ro";
            active->push_back(r);
        }
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
    auto cur = active;
    if (cur.empty())
        cur = find_mounts_for_device(device_path);
    std::sort(cur.begin(), cur.end(),
              [](const MountRecord &a, const MountRecord &b) {
                  return a.target.size() > b.target.size();
              });
    for (const auto &m : cur) {
        unmount_one(m, nullptr);
        if (temp_created && m.target.find("/tmp/reflash-mnt-") == 0)
            rmdir(m.target.c_str());
    }

    if (original.empty())
        return true;

    return remount_records(original, err);
}

static std::string fs_fuse_hint(FsType t) {
    switch (t) {
    case FsType::Ext2:
    case FsType::Ext3:
    case FsType::Ext4:
        return "ext";
    case FsType::Fat12:
    case FsType::Fat16:
    case FsType::Fat32:
        return "vfat";
    case FsType::ExFat:
        return "exfat";
    case FsType::Ntfs:
        return "ntfs";
    default:
        return "auto";
    }
}

static bool mkdtemp_mnt(char *tmpl, std::string *err) {
    std::snprintf(tmpl, 64, "/tmp/reflash-mnt-XXXXXX");
    if (!mkdtemp(tmpl)) {
        if (err)
            *err = "mkdtemp failed";
        return false;
    }
    return true;
}

bool mount_for_browse(const std::string &device_path, std::vector<MountRecord> *active,
                      bool *temp_created, std::string *err) {
    if (temp_created)
        *temp_created = false;
    if (active)
        active->clear();
    if (device_path.empty()) {
        if (err)
            *err = "no drive selected";
        return false;
    }
    if (path_is_directory(device_path)) {
        MountRecord r;
        r.device = device_path;
        r.target = device_path;
        r.fstype = "dir";
        r.options = "bind";
        if (active)
            active->push_back(r);
        return true;
    }

    auto cur = find_mounts_for_device(device_path);
    if (!cur.empty()) {
        if (active)
            *active = cur;
        return true;
    }

    Device dev;
    if (!dev.open(device_path)) {
        if (err)
            *err = "cannot open " + device_path;
        return false;
    }

    struct Candidate {
        std::uint64_t offset = 0;
        std::string hint;
        std::string part_path; /* block partition path when available */
    };
    std::vector<Candidate> cands;

    {
        std::string derr;
        FsType ft = detect_fs(dev, 0, &derr);
        if (ft != FsType::Unknown)
            cands.push_back({0, fs_fuse_hint(ft), {}});
    }

    PartitionMap pm;
    probe_partitions(dev, pm, nullptr);
    for (const auto &p : pm.parts) {
        std::string derr;
        FsType ft = detect_fs(dev, p.start_bytes, &derr);
        Candidate c;
        c.offset = p.start_bytes;
        c.hint = ft == FsType::Unknown ? "auto" : fs_fuse_hint(ft);
        /* /dev/sda + part 1 => /dev/sda1 when device is a real disk name */
        if (path_is_block(device_path) && device_path.find("/dev/") == 0 &&
            device_path.find_first_of("0123456789", 5) == std::string::npos) {
            c.part_path = device_path + std::to_string(p.index);
        }
        cands.push_back(c);
    }
    if (cands.empty())
        cands.push_back({0, "auto", {}});

    dev.close();

    std::string last_err;
    for (const auto &c : cands) {
        /* Prefer udisks on a concrete partition node. */
        if (!c.part_path.empty() && path_is_block(c.part_path) && which_ok("udisksctl")) {
            MountRecord rec;
            std::string e;
            if (try_udisks_mount_block(c.part_path, "ro", &rec, &e)) {
                if (active)
                    active->push_back(rec);
                if (temp_created)
                    *temp_created = true;
                return true;
            }
            last_err = e;
        }

        if (path_is_block(device_path) && c.offset == 0 && !no_auth_target(device_path) &&
            which_ok("udisksctl")) {
            MountRecord rec;
            std::string e;
            if (try_udisks_mount_block(device_path, "ro", &rec, &e)) {
                if (active)
                    active->push_back(rec);
                if (temp_created)
                    *temp_created = true;
                return true;
            }
            last_err = e;
        }

        if (no_auth_target(device_path) || path_is_block(device_path)) {
            char tmpl[64];
            if (!mkdtemp_mnt(tmpl, err))
                return false;
            MountRecord rec;
            std::string e;
            if (try_fuse_mount_file(device_path, tmpl, c.hint, "ro", c.offset, &rec, &e)) {
                if (active)
                    active->push_back(rec);
                if (temp_created)
                    *temp_created = true;
                return true;
            }
            last_err = e;
            rmdir(tmpl);
        }
    }

    if (err)
        *err = last_err.empty() ? ("could not mount " + device_path + " for browsing") : last_err;
    return false;
}

std::string find_builtin_manifest_db(const std::string &device_path) {
    auto try_root = [](const std::string &root) -> std::string {
        if (root.empty())
            return {};
        std::string p = root;
        if (p.back() == '/')
            p.pop_back();
        p += "/manifest.db";
        if (::access(p.c_str(), R_OK) == 0)
            return p;
        return {};
    };

    if (path_is_directory(device_path)) {
        std::string hit = try_root(device_path);
        if (!hit.empty())
            return hit;
    }

    for (const auto &m : find_mounts_for_device(device_path)) {
        std::string hit = try_root(m.target);
        if (!hit.empty())
            return hit;
    }
    return {};
}

} /* namespace reflash */
