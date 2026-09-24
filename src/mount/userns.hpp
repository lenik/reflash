/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <string>

namespace sdmsg {

/* True after this process successfully entered a user+mount namespace. */
bool in_user_mount_ns();

/*
 * Enter a new user namespace (map current uid/gid -> 0) and a new mount
 * namespace (make mounts private). Idempotent.
 *
 * Call when host FUSE mount fails with EPERM so fuse2fs/ntfs-3g can
 * mount() with CAP_SYS_ADMIN inside the namespace. After success, FUSE
 * mounts created by this process are visible to Browser/SHA-1 in-process.
 */
bool ensure_user_mount_ns(std::string *err = nullptr);

} /* namespace sdmsg */
