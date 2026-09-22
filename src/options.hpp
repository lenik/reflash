/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>

namespace sdmsg {

enum class RunMode { Linear, Recursive };
enum class Action { Massage, Test };

struct Options {
    std::string target;
    std::string sqlite_db;
    std::uint64_t block_size = 0; /* 0 = auto */
    RunMode mode = RunMode::Linear;
    Action action = Action::Massage;
    bool auto_mount = false;
    bool gui = false;
    int verbosity = 0; /* -quiet .. +verbose */
};

bool parse_options(int argc, char **argv, Options &out);
void print_usage(FILE *out);
void print_version();

} /* namespace sdmsg */
