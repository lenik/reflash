/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "options.hpp"
#include "engine/massage.hpp"
#include "gui/app.hpp"
#include "mount/host_priv.hpp"
#include "mount/userns.hpp"
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

namespace reflash {

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

} /* namespace reflash */

int main(int argc, char **argv) {
    const char *exe = self_exe();
    (void)exe;
    init_i18n(LOCALEDIR);

    reflash::Options opts;
    if (!reflash::parse_options(argc, argv, opts))
        return 2;

    /*
     * Host privilege agent must start before the user+mount namespace: pkexec
     * breaks inside the userns. Mark GUI before fork so the agent never falls
     * back to an invisible sudo password prompt.
     */
    if (opts.gui)
        reflash::set_host_priv_gui(true);
    if (!reflash::start_host_priv_agent() && opts.verbosity > 0)
        fprintf(stderr, "reflash: host privilege agent failed to start\n");

    /*
     * Enter a user+mount namespace while still single-threaded so later FUSE
     * mounts (fuse2fs, …) can retry with CAP_SYS_ADMIN in-ns after host
     * fusermount EPERM. Must happen before wx/worker threads (unshare NEWUSER
     * then returns EINVAL). Sets GIO_USE_VFS=local to avoid GVFS D-Bus noise.
     */
    {
        std::string ns_err;
        if (!reflash::ensure_user_mount_ns(&ns_err) && opts.verbosity > 0)
            fprintf(stderr, "reflash: user mount namespace: %s\n", ns_err.c_str());
    }

    auto engine = std::make_shared<reflash::MassageEngine>(opts);

    bool want_gui = opts.gui && opts.action != reflash::Action::Test;
    if (want_gui && !reflash::display_available()) {
        fprintf(stderr, "reflash: --gui requested but no display available; running headless\n");
        want_gui = false;
    }

    if (want_gui)
        return reflash::run_gui(opts, argc, argv);

    if (opts.target.empty()) {
        fprintf(stderr, "reflash: missing DEVICE/FILE\n");
        reflash::print_usage(stderr);
        return 2;
    }

    std::string err;
    if (!engine->start(&err)) {
        fprintf(stderr, "reflash: %s\n", err.c_str());
        return 1;
    }
    reflash::headless_progress_loop(*engine);
    engine->join();
    return engine->exit_code();
}
