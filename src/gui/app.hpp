/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/massage.hpp"

#include <memory>

namespace sdmsg {

int run_gui(std::shared_ptr<MassageEngine> engine, int &argc, char **argv);

} /* namespace sdmsg */
