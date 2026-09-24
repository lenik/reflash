/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <string>
#include <vector>

namespace sdmsg {

struct MountRecord {
    std::string device;
    std::string target;
    std::string fstype;
    std::string options;
    /* If set, device came from udisksctl loop-setup of this image. */
    std::string loop_from_image;
    bool fuse_mount = false;
    bool read_only() const;
};

std::vector<MountRecord> find_mounts_for_device(const std::string &device_path);
bool path_is_mounted(const std::string &device_path);
bool path_is_block(const std::string &path);
bool path_is_directory(const std::string &path);
bool no_auth_target(const std::string &path);

bool unmount_device(const std::string &device_path, std::string *err);
bool remount_records(const std::vector<MountRecord> &recs, std::string *err);

/*
 * Unmount the device/image (FUSE or block). Optical media may also run eject(1).
 * Does not power off the drive.
 */
bool eject_device(const std::string &device_path, std::string *err = nullptr);

/*
 * Unmount then ask the system to power off the drive (udisksctl power-off)
 * so it can be unplugged. Image/loop targets only unmount.
 */
bool safely_remove_device(const std::string &device_path, std::string *err = nullptr);

/*
 * Regular files and loop devices: FUSE only.
 * Real disks: udisksctl, then mount, pkexec, sudo.
 * fakeroot cannot grant CAP_SYS_ADMIN; it does not help here.
 */
bool mount_device(const std::string &device, const std::string &target, const std::string &fstype,
                  const std::string &options, std::string *err);

/* Snapshot mounts, then unmount so rewrite can proceed. */
bool ensure_unmounted(const std::string &device_path, std::vector<MountRecord> *original,
                      std::string *err);

/*
 * After rewrite: get a readable mount for SHA-1. No terminal prompt.
 * - If already mounted: use existing mounts (return them in *active).
 * - If *original is non-empty: remount those read-only for hashing, then the
 *   caller restores the original options.
 * - Regular files and loop devices: FUSE read-only.
 * - Real disks: udisks, then mount/sudo/pkexec.
 * *temp_created is true if we created a temporary mount that must be undone
 * before restoring *original.
 */
bool prepare_sha1_mounts(const std::string &device_path, const std::vector<MountRecord> &original,
                         std::vector<MountRecord> *active, bool *temp_created, std::string *err);

/*
 * Mount a drive/image so the File Browser can read it.
 * Prefers an existing mount; otherwise FUSE (files/loop, with partition offset
 * when needed) or udisks (real disks/partitions).
 */
bool mount_for_browse(const std::string &device_path, std::vector<MountRecord> *active,
                      bool *temp_created, std::string *err);

/*
 * If the device/directory is already mounted (or is a directory), return the
 * path to <mount-root>/manifest.db when that file exists. Empty otherwise.
 */
std::string find_builtin_manifest_db(const std::string &device_path);

/* Restore to the snapshotted original mount state (RW if it was RW). */
bool restore_mount_state(const std::string &device_path, const std::vector<MountRecord> &original,
                         const std::vector<MountRecord> &active, bool temp_created,
                         std::string *err);

} /* namespace sdmsg */
