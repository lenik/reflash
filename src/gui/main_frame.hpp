/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/massage.hpp"
#include "gui/grid_panel.hpp"
#include "gui/log_panel.hpp"
#include "options.hpp"
#include "db/store.hpp"
#include "io/devices.hpp"

#include <memory>
#include <string>
#include <vector>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/statbmp.h>
#include <wx/statusbr.h>
#include <wx/timer.h>
#include <wx/toolbar.h>

namespace sdmsg {

class BrowserFrame;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(Options opts);

    void open_target(const std::string &path, const std::string &fstype_hint = {});
    void close_target();
    bool manifest_on_target() const;
    void eject_or_remove(bool power_off);
    const Options &options() const { return opts_; }
    size_t grid_cells() const;
    size_t grid_status_count(CellStatus st) const;
    size_t log_count() const;
    unsigned grid_resets() const { return grid_resets_; }

private:
    Options opts_;
    std::shared_ptr<MassageEngine> engine_;
    wxGauge *gauge_ = nullptr;
    GridPanel *grid_ = nullptr;
    LogPanel *log_ = nullptr;
    wxMenu *file_disks_ = nullptr;
    wxMenu *file_loops_ = nullptr;
    wxMenu *file_recents_ = nullptr;
    wxToolBar *toolbar_ = nullptr;
    wxStaticBitmap *work_icon_ = nullptr;
    wxStaticBitmap *field_icons_[8]{}; /* indexed by StatusField; SF_MSG/SF_ICON unused */
    wxTimer timer_;
    bool paused_ = false;
    bool offered_bad_ = false;
    bool db_open_ = false;
    bool db_cli_explicit_ = false;
    bool target_fs_ok_ = false;
    std::string target_fstype_;
    unsigned grid_resets_ = 0;
    Store catalog_;
    BrowserFrame *browser_ = nullptr;
    std::vector<BlockDevInfo> device_list_;

    void build_menubar();
    void build_toolbar();
    void build_statusbar();
    void place_work_icon();
    void place_field_icons();
    void set_work_state(int state);
    wxStaticBitmap *make_field_icon(wxStatusBar *bar, const char *art);
    void fill_drive_menu(wxMenu *disks, wxMenu *loops, wxMenu *recents);
    void rebuild_drive_menus();
    void popup_drive_menu();
    void update_title();
    void set_field(int index, const wxString &text);
    void set_status(const wxString &text);
    void refresh_db_status();
    void refresh_target_fs(const std::string &fstype_hint);
    void update_drive_ui();
    void update_db_ui();
    void open_database(const std::string &path);
    void close_database();
    /* Prefer device-root manifest.db when CLI did not pin -d. */
    void maybe_open_builtin_manifest();
    void set_running_ui(bool running);
    void set_pause_label(bool is_paused);
    bool ensure_engine();
    void start_run();
    void save_session() const;

    void on_drive_tool(wxCommandEvent &);
    void on_manifest_tool(wxCommandEvent &);
    void on_image_file(wxCommandEvent &);
    void on_close_target(wxCommandEvent &);
    void on_eject_target(wxCommandEvent &);
    void on_safely_remove(wxCommandEvent &);
    void on_quit(wxCommandEvent &);
    void on_device_pick(wxCommandEvent &);
    void on_recent_pick(wxCommandEvent &);
    void on_db_open(wxCommandEvent &);
    void on_db_create_default(wxCommandEvent &);
    void on_db_close(wxCommandEvent &);
    void on_db_integrity(wxCommandEvent &);
    void on_db_browser(wxCommandEvent &);
    void on_db_admin(wxCommandEvent &);
    void on_db_user_mounts(wxCommandEvent &);
    void on_view_log(wxCommandEvent &);
    void on_edit_raw_disk(wxCommandEvent &);
    void on_edit_on_files(wxCommandEvent &);
    void on_edit_dry_run(wxCommandEvent &);
    void on_edit_write_cache(wxCommandEvent &);
    void on_edit_verify_writes(wxCommandEvent &);
    void on_log_level(wxCommandEvent &);
    void on_edit_prefs(wxCommandEvent &);
    void on_help_about(wxCommandEvent &);
    void on_help_usage(wxCommandEvent &);
    void on_start(wxCommandEvent &);
    void on_pause(wxCommandEvent &);
    void on_stop(wxCommandEvent &);
    void on_timer(wxTimerEvent &);
    void maybe_offer_badblocks();

    bool user_mounts_ready() const;
    void refresh_user_mounts_ui();
    bool show_user_mounts_dialog();
};

} /* namespace sdmsg */
