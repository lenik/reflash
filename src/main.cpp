/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "options.hpp"
#include "engine/massage.hpp"
#include "gui/app.hpp"
#include "config.h"

extern "C" {
#include <bas/locale/i18n.h>
#include <bas/log/deflog.h>
#include <bas/proc/env.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

define_logger();

namespace sdmsg {

static bool display_available() {
    const char *d = getenv("DISPLAY");
    if (d && d[0])
        return true;
    const char *w = getenv("WAYLAND_DISPLAY");
    if (w && w[0])
        return true;
    return false;
}

static void headless_progress_loop(MassageEngine &eng) {
    while (eng.running() || !eng.progress().snapshot().finished) {
        auto snap = eng.progress().snapshot();
        for (const auto &e : eng.progress().take_logs())
            fprintf(stderr, "%s\n", e.message.c_str());
        if (snap.bytes_total) {
            double pct = 100.0 * static_cast<double>(snap.bytes_done) / snap.bytes_total;
            fprintf(stderr, "\r%s %.1f%% %.1f MiB/s ETA %.0fs%s   ", snap.phase.c_str(), pct,
                    snap.bytes_per_sec / (1024.0 * 1024.0), snap.eta_seconds,
                    snap.paused ? " PAUSED" : "");
            fflush(stderr);
        }
        if (snap.finished)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    fprintf(stderr, "\n");
}

} /* namespace sdmsg */

int main(int argc, char **argv) {
    const char *exe = self_exe();
    (void)exe;
    init_i18n(LOCALEDIR);

    sdmsg::Options opts;
    if (!sdmsg::parse_options(argc, argv, opts))
        return 2;

    auto engine = std::make_shared<sdmsg::MassageEngine>(opts);

    bool want_gui = opts.gui && opts.action != sdmsg::Action::Test;
    if (want_gui && !sdmsg::display_available()) {
        fprintf(stderr, "sdmsg: --gui requested but no display available; running headless\n");
        want_gui = false;
    }

    if (want_gui)
        return sdmsg::run_gui(engine, argc, argv);

    std::string err;
    if (!engine->start(&err)) {
        fprintf(stderr, "sdmsg: %s\n", err.c_str());
        return 1;
    }
    sdmsg::headless_progress_loop(*engine);
    engine->join();
    return engine->exit_code();
}
