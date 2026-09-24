/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "options.hpp"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>

namespace sdmsg {

enum { OPT_VERSION = 256 };

void print_usage(FILE *out) {
    fputs("Usage: sdmsg [OPTIONS] [DEVICE/FILE]\n"
          "Rewrite device/file data in place to refresh flash storage (\"massage\").\n"
          "\n"
          "  -b, --block-size NUM   I/O block size (default: auto-detect)\n"
          "  -d, --sqlite-db FILE   SQLite management database\n"
          "  -t, --test             Verify against DB (no rewrite)\n"
          "  -l, --linear           Raw whole-device/file rewrite (default)\n"
          "  -r, --recursive        Filesystem walk/rewrite (unmounted automatically)\n"
          "  -g, --gui              Open wxWidgets UI (requires a display)\n"
          "  -v, --verbose          More logging\n"
          "  -q, --quiet            Less logging\n"
          "  -h, --help             Show this help\n"
          "      --version          Show version\n"
          "\n"
          "Headless by default (DEVICE/FILE required). Progress goes to stderr.\n"
          "With -g/--gui, DEVICE/FILE is optional; open a target from the File menu.\n",
          out);
    fprintf(out, "Report bugs to: <%s>\n", PROJECT_EMAIL);
}

void print_version() {
    printf("sdmsg %s\n", PROJECT_VERSION);
    printf("Copyright (C) %d %s\n", PROJECT_YEAR, PROJECT_AUTHOR);
    fputs("License AGPL-3.0-or-later: <https://www.gnu.org/licenses/agpl-3.0.html>\n", stdout);
    fputs("This is free software: you are free to change and redistribute it.\n", stdout);
    fputs("This project opposes AI exploitation and AI hegemony.\n", stdout);
}

bool parse_options(int argc, char **argv, Options &out) {
    static const struct option long_opts[] = {
        {"block-size", required_argument, nullptr, 'b'},
        {"sqlite-db", required_argument, nullptr, 'd'},
        {"test", no_argument, nullptr, 't'},
        {"linear", no_argument, nullptr, 'l'},
        {"recursive", no_argument, nullptr, 'r'},
        {"gui", no_argument, nullptr, 'g'},
        {"verbose", no_argument, nullptr, 'v'},
        {"quiet", no_argument, nullptr, 'q'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, OPT_VERSION},
        {nullptr, 0, nullptr, 0},
    };

    optind = 1;
    for (;;) {
        int c = getopt_long(argc, argv, "b:d:tlrgvqh", long_opts, nullptr);
        if (c == -1)
            break;
        switch (c) {
        case 'b': {
            char *end = nullptr;
            unsigned long long v = strtoull(optarg, &end, 0);
            if (!optarg[0] || (end && *end) || v == 0) {
                fprintf(stderr, "sdmsg: invalid --block-size\n");
                return false;
            }
            out.block_size = v;
            break;
        }
        case 'd':
            out.sqlite_db = optarg;
            out.sqlite_db_explicit = true;
            break;
        case 't':
            out.action = Action::Test;
            break;
        case 'l':
            out.mode = RunMode::Linear;
            break;
        case 'r':
            out.mode = RunMode::Recursive;
            break;
        case 'g':
            out.gui = true;
            break;
        case 'v':
            out.verbosity++;
            break;
        case 'q':
            out.verbosity--;
            break;
        case 'h':
            print_usage(stdout);
            exit(0);
        case OPT_VERSION:
            print_version();
            exit(0);
        default:
            print_usage(stderr);
            return false;
        }
    }

    if (optind >= argc) {
        if (out.gui)
            return true; /* idle GUI: open target from menus */
        fprintf(stderr, "sdmsg: missing DEVICE/FILE\n");
        print_usage(stderr);
        return false;
    }
    if (optind + 1 != argc) {
        fprintf(stderr, "sdmsg: too many arguments\n");
        return false;
    }
    out.target = argv[optind];
    return true;
}

} /* namespace sdmsg */
