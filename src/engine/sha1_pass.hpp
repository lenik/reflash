/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "db/store.hpp"
#include "engine/progress.hpp"
#include "mount/automount.hpp"

#include <string>
#include <vector>

namespace reflash {

/* Hash regular files under each mount root; update Store SHA-1 / verify status.
 * Relative DB paths like "/foo.txt" are matched case-insensitively when needed.
 * verify_mode: compare to existing SHA-1 and bump verify_*; else set_sha1 only.
 */
bool sha1_mounted_files(Store &store, const std::vector<MountRecord> &mounts, Progress &progress,
                        bool verify_mode, std::string *err);

} /* namespace reflash */
