/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "fs/detect.hpp"

namespace sdmsg {

bool scan_ext(Device &dev, std::uint64_t base, FsType type, FsScanResult &out);

} /* namespace sdmsg */
