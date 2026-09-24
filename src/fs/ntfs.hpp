/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "fs/detect.hpp"

namespace reflash {

bool scan_ntfs(Device &dev, std::uint64_t base, FsScanResult &out);

} /* namespace reflash */
