/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "options.hpp"

#include <string>
#include <vector>

namespace reflash {

/* ~/.config/reflash (REFLASH_CONFIG overrides the directory). */
std::string config_dir();

/* Last GUI target, database, mode, and SHA-1 window.
 * REFLASH_SESSION overrides the path (a file, not a directory). */
std::string gui_session_path();

/* Fill empty CLI fields from the session file. Missing paths are ignored. */
void restore_gui_session(Options &opts, bool keep_target, bool keep_db);

void save_gui_session(const Options &opts);

std::vector<std::string> load_recent_targets(size_t max_n = 12);
void remember_recent_target(const std::string &path, size_t max_n = 12);

/* Conflict resolution preference. action: 0 delete, 1 overwrite, 2 ignore; -1 = ask. */
void load_conflict_pref(int *action, bool *remember);
void save_conflict_pref(int action, bool remember);

/* Browser view + preference settings (persisted under config_dir). */
struct BrowserSettings {
    /* 0=icon, 1=list, 2=compact */
    int view_mode = 1;
    /* 0=manual, 1=name, 2=size, 3=type, 4=mtime */
    int arrange = 1;
    bool arrange_rev = false;
    bool show_hidden = false;
    bool show_thumbs = false;
    int zoom_level = 0;
    /* Months before a stored SHA-1 / rewrite is considered stale. Default 6. */
    int auto_rewrite_months = 6;
};

BrowserSettings load_browser_settings();
void save_browser_settings(const BrowserSettings &s);

} /* namespace reflash */
