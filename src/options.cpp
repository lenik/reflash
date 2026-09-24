/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "options.hpp"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>

extern "C" {
#include <bas/locale/i18n.h>
}

namespace reflash {

enum { OPT_VERSION = 256 };

void print_usage(FILE *out) {
    /* Help text is gettext'd line-by-line. Option tokens stay outside _(). */
    fputs(_("Usage: reflash [OPTIONS] [DEVICE/FILE]\n"), out);
    fputs(_("Rewrite device/file data in place to refresh flash storage (\"massage\").\n"), out);
    fputs("\n", out);
    fputs("  -b, --block-size NUM   ", out);
    fputs(_("I/O block size (default: auto-detect)\n"), out);
    fputs("  -d, --sqlite-db FILE   ", out);
    fputs(_("SQLite management database\n"), out);
    fputs("  -t, --test             ", out);
    fputs(_("Verify against DB (no rewrite)\n"), out);
    fputs("  -l, --linear           ", out);
    fputs(_("Raw whole-device/file rewrite (default)\n"), out);
    fputs("  -r, --recursive        ", out);
    fputs(_("Filesystem walk/rewrite (unmounted automatically)\n"), out);
    fputs("  -g, --gui              ", out);
    fputs(_("Open wxWidgets UI (requires a display)\n"), out);
    fputs("  -v, --verbose          ", out);
    fputs(_("More logging\n"), out);
    fputs("  -q, --quiet            ", out);
    fputs(_("Less logging\n"), out);
    fputs("  -h, --help             ", out);
    fputs(_("Show this help\n"), out);
    fputs("      --version          ", out);
    fputs(_("Show version\n"), out);
    fputs("\n", out);
    fputs(_("Headless by default (DEVICE/FILE required). Progress goes to stderr.\n"), out);
    fputs(_("With -g/--gui, DEVICE/FILE is optional; open a target from the File menu.\n"), out);
    fprintf(out, _("Report bugs to: <%s>\n"), PROJECT_EMAIL);
}

void print_version() {
    printf(_("reflash %s\n"), PROJECT_VERSION);
    printf(_("Copyright (C) %d %s\n"), PROJECT_YEAR, PROJECT_AUTHOR);
    fputs(_("License AGPL-3.0-or-later: <https://www.gnu.org/licenses/agpl-3.0.html>\n"), stdout);
    fputs(_("This is free software: you are free to change and redistribute it.\n"), stdout);
    fputs(_("This project opposes AI exploitation and AI hegemony.\n"), stdout);
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
                fprintf(stderr, _("reflash: invalid --block-size\n"));
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
        fprintf(stderr, _("reflash: missing DEVICE/FILE\n"));
        print_usage(stderr);
        return false;
    }
    if (optind + 1 != argc) {
        fprintf(stderr, _("reflash: too many arguments\n"));
        return false;
    }
    out.target = argv[optind];
    return true;
}

} /* namespace reflash */
