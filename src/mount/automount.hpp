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
    bool read_only() const;
};

std::vector<MountRecord> find_mounts_for_device(const std::string &device_path);
bool path_is_mounted(const std::string &device_path);

bool unmount_device(const std::string &device_path, std::string *err);
bool remount_records(const std::vector<MountRecord> &recs, std::string *err);

/* Mount with optional privilege escalation (mount → pkexec → sudo). */
bool mount_device(const std::string &device, const std::string &target, const std::string &fstype,
                  const std::string &options, std::string *err);

/* Ensure rewrite can proceed unmounted. Snapshots mounts into *original. */
bool ensure_unmounted(const std::string &device_path, bool auto_mount,
                      std::vector<MountRecord> *original, std::string *err);

enum class Sha1MountChoice { Skip, MountRo };

/*
 * After rewrite: get a readable mount for SHA-1.
 * - If already mounted: use existing mounts (return them in *active).
 * - If auto_mount and we have *original: remount those (prefer RO for hashing
 *   when possible, then caller restores original).
 * - Else: prompt (or use SDMSG_SHA1=skip|mount) then mount RO via sudo/pkexec.
 * *temp_created is true if we created a temporary mount that must be undone
 * before restoring *original.
 */
bool prepare_sha1_mounts(const std::string &device_path, bool auto_mount, bool interactive,
                         const std::vector<MountRecord> &original,
                         std::vector<MountRecord> *active, bool *temp_created, std::string *err);

/* Restore to the snapshotted original mount state (RW if it was RW). */
bool restore_mount_state(const std::string &device_path, const std::vector<MountRecord> &original,
                         const std::vector<MountRecord> &active, bool temp_created,
                         std::string *err);

Sha1MountChoice prompt_sha1_mount_choice(bool interactive);

} /* namespace sdmsg */
