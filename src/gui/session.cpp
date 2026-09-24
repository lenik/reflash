/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/session.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <wx/fileconf.h>
#include <wx/string.h>

namespace reflash {

namespace {

void mkdir_p(const std::string &dir) {
    if (dir.empty() || dir == "/")
        return;
    auto mid = dir.find_last_of('/');
    if (mid != std::string::npos && mid > 0)
        mkdir_p(dir.substr(0, mid));
    ::mkdir(dir.c_str(), 0700);
}

void mkdir_p_parent(const std::string &file) {
    auto slash = file.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return;
    mkdir_p(file.substr(0, slash));
}

bool path_exists(const std::string &path) {
    return !path.empty() && ::access(path.c_str(), F_OK) == 0;
}

std::string legacy_gui_session_path() {
    const char *home = std::getenv("HOME");
    if (!home || !home[0])
        return {};
    return std::string(home) + "/.config/reflash/gui.ini";
}

} /* namespace */

std::string config_dir() {
    if (const char *env = std::getenv("REFLASH_CONFIG")) {
        if (env[0]) {
            mkdir_p(env);
            return env;
        }
    }
    const char *home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.config/reflash" : "/tmp/reflash";
    if (home)
        mkdir_p(std::string(home) + "/.config");
    mkdir_p(dir);
    return dir;
}

std::string gui_session_path() {
    if (const char *env = std::getenv("REFLASH_SESSION")) {
        if (env[0])
            return env;
    }
    return config_dir() + "/reflash.ini";
}

void restore_gui_session(Options &opts, bool keep_target, bool keep_db) {
    std::string path = gui_session_path();
    if (!path_exists(path)) {
        /* One-time migration from the old reflash config location. */
        std::string legacy = legacy_gui_session_path();
        if (path_exists(legacy))
            path = legacy;
        else
            return;
    }
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    if (!keep_target) {
        wxString target;
        if (cfg.Read("target", &target)) {
            std::string t = target.ToStdString();
            if (path_exists(t))
                opts.target = t;
        }
    }
    if (!keep_db) {
        wxString db;
        if (cfg.Read("database", &db)) {
            std::string d = db.ToStdString();
            if (path_exists(d))
                opts.sqlite_db = d;
        }
    }
    if (!keep_target) {
        wxString mode;
        if (cfg.Read("mode", &mode)) {
            if (mode == "recursive")
                opts.mode = RunMode::Recursive;
            else if (mode == "linear")
                opts.mode = RunMode::Linear;
        }
    }
    long days = 0;
    if (cfg.Read("sha1_valid_days", &days) && days > 0 && days <= 3650)
        opts.sha1_valid_days = static_cast<int>(days);
    long dry = 0;
    if (cfg.Read("dry_run", &dry))
        opts.dry_run = dry != 0;
    wxString wcm;
    if (cfg.Read("write_cache", &wcm)) {
        if (wcm == "writethrough" || wcm == "write_through")
            opts.write_cache = WriteCacheMode::WriteThrough;
        else if (wcm == "mincore")
            opts.write_cache = WriteCacheMode::Mincore;
        else
            opts.write_cache = WriteCacheMode::Cachestat;
    } else {
        long wt = 0;
        if (cfg.Read("write_through", &wt) && wt != 0)
            opts.write_cache = WriteCacheMode::WriteThrough;
    }
    long vw = 0;
    if (cfg.Read("verify_writes", &vw))
        opts.verify_writes = vw != 0;
    long poll = 0;
    if (cfg.Read("flush_poll_ms", &poll) && poll >= 50 && poll <= 60000)
        opts.flush_poll_ms = static_cast<int>(poll);
    long bs = 0;
    if (cfg.Read("block_size", &bs) && bs >= 0)
        opts.block_size = static_cast<std::uint64_t>(bs);
}

void save_gui_session(const Options &opts) {
    std::string path = gui_session_path();
    mkdir_p_parent(path);
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    cfg.Write("target", wxString::FromUTF8(opts.target));
    cfg.Write("database", wxString::FromUTF8(opts.sqlite_db));
    cfg.Write("mode", opts.mode == RunMode::Recursive ? "recursive" : "linear");
    cfg.Write("sha1_valid_days", opts.sha1_valid_days);
    cfg.Write("dry_run", opts.dry_run ? 1 : 0);
    const char *wcm = "cachestat";
    if (opts.write_cache == WriteCacheMode::WriteThrough)
        wcm = "writethrough";
    else if (opts.write_cache == WriteCacheMode::Mincore)
        wcm = "mincore";
    cfg.Write("write_cache", wxString(wcm));
    cfg.Write("verify_writes", opts.verify_writes ? 1 : 0);
    cfg.Write("flush_poll_ms", opts.flush_poll_ms);
    cfg.Write("block_size", static_cast<long>(opts.block_size));
    cfg.Flush();
}

std::vector<std::string> load_recent_targets(size_t max_n) {
    std::vector<std::string> out;
    std::string path = gui_session_path();
    if (!path_exists(path)) {
        std::string legacy = legacy_gui_session_path();
        if (path_exists(legacy))
            path = legacy;
        else
            return out;
    }
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    long n = 0;
    cfg.Read("recent/count", &n, 0);
    for (long i = 0; i < n && out.size() < max_n; ++i) {
        wxString p;
        if (!cfg.Read(wxString::Format("recent/path%ld", i), &p))
            continue;
        std::string s = p.ToStdString();
        if (path_exists(s))
            out.push_back(s);
    }
    return out;
}

void remember_recent_target(const std::string &path, size_t max_n) {
    if (path.empty() || !path_exists(path))
        return;
    auto list = load_recent_targets(max_n);
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > max_n)
        list.resize(max_n);
    std::string conf = gui_session_path();
    mkdir_p_parent(conf);
    wxFileConfig cfg("reflash", wxEmptyString, conf, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    cfg.Write("recent/count", static_cast<long>(list.size()));
    for (size_t i = 0; i < list.size(); ++i)
        cfg.Write(wxString::Format("recent/path%zu", i), wxString::FromUTF8(list[i]));
    cfg.Flush();
}

void load_conflict_pref(int *action, bool *remember) {
    if (action)
        *action = -1;
    if (remember)
        *remember = false;
    std::string path = gui_session_path();
    if (!path_exists(path)) {
        std::string legacy = legacy_gui_session_path();
        if (path_exists(legacy))
            path = legacy;
        else
            return;
    }
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    long rem = 0;
    cfg.Read("conflict/remember", &rem, 0);
    if (remember)
        *remember = rem != 0;
    long act = -1;
    cfg.Read("conflict/action", &act, -1);
    if (action)
        *action = static_cast<int>(act);
}

void save_conflict_pref(int action, bool remember) {
    std::string path = gui_session_path();
    mkdir_p_parent(path);
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    cfg.Write("conflict/remember", remember ? 1L : 0L);
    cfg.Write("conflict/action", static_cast<long>(action));
    cfg.Flush();
}

static std::string browser_settings_path() { return config_dir() + "/browser.ini"; }

BrowserSettings load_browser_settings() {
    BrowserSettings s;
    std::string path = browser_settings_path();
    if (!path_exists(path))
        return s;
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    long v = s.view_mode;
    if (cfg.Read("view/mode", &v) && v >= 0 && v <= 2)
        s.view_mode = static_cast<int>(v);
    v = s.arrange;
    if (cfg.Read("view/arrange", &v) && v >= 0 && v <= 4)
        s.arrange = static_cast<int>(v);
    long b = 0;
    if (cfg.Read("view/arrange_rev", &b))
        s.arrange_rev = b != 0;
    b = 0;
    if (cfg.Read("view/show_hidden", &b))
        s.show_hidden = b != 0;
    b = 0;
    if (cfg.Read("view/show_thumbs", &b))
        s.show_thumbs = b != 0;
    v = s.zoom_level;
    if (cfg.Read("view/zoom", &v) && v >= -2 && v <= 3)
        s.zoom_level = static_cast<int>(v);
    v = s.auto_rewrite_months;
    if (cfg.Read("prefs/auto_rewrite_months", &v) && v >= 1 && v <= 120)
        s.auto_rewrite_months = static_cast<int>(v);
    return s;
}

void save_browser_settings(const BrowserSettings &s) {
    std::string path = browser_settings_path();
    mkdir_p_parent(path);
    wxFileConfig cfg("reflash", wxEmptyString, path, wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    cfg.Write("view/mode", static_cast<long>(s.view_mode));
    cfg.Write("view/arrange", static_cast<long>(s.arrange));
    cfg.Write("view/arrange_rev", s.arrange_rev ? 1L : 0L);
    cfg.Write("view/show_hidden", s.show_hidden ? 1L : 0L);
    cfg.Write("view/show_thumbs", s.show_thumbs ? 1L : 0L);
    cfg.Write("view/zoom", static_cast<long>(s.zoom_level));
    cfg.Write("prefs/auto_rewrite_months", static_cast<long>(s.auto_rewrite_months));
    cfg.Flush();
}

} /* namespace reflash */
