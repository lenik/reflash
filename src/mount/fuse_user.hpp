/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <string>

namespace sdmsg {

struct FuseUserMountStatus {
    bool fuse_group_exists = false;
    bool user_in_fuse_group = false;
    bool fusermount_setuid = false;
    bool fusermount_cap = false;
    std::string fusermount_path;

    /* Ready when the mount helper can elevate (setuid and/or capability). */
    bool ready() const { return fusermount_setuid || fusermount_cap; }
};

struct FuseUserMountDesire {
    bool fuse_group = true;
    bool member = false;
    bool setuid = false;
    bool cap = false;
};

FuseUserMountStatus probe_fuse_user_mounts();

/* Apply checkbox state via pkexec/sudo. No-op (success) when already matched. */
bool apply_fuse_user_mounts(const FuseUserMountDesire &desire, std::string *err = nullptr);

/* Drop membership / setuid / caps; leave the fuse group itself. */
bool disable_fuse_user_mounts(std::string *err = nullptr);

} /* namespace sdmsg */
