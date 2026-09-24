/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/main_frame.hpp"
#include "config.h"
#include "db/store.hpp"
#include "gui/alert.hpp"
#include "gui/browser.hpp"
#include "gui/session.hpp"
#include "fs/detect.hpp"
#include "io/device.hpp"
#include "io/devices.hpp"
#include "mount/automount.hpp"
#include "mount/fuse_user.hpp"
#include "mount/userns.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <wx/aboutdlg.h>
#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/cursor.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/textctrl.h>
#include <wx/process.h>
#include <wx/utils.h>


namespace reflash {

namespace {

enum {
    ID_DRIVE = wxID_HIGHEST + 1,
    ID_MANIFEST,
    ID_IMAGE_FILE,
    ID_CLOSE_TARGET,
    ID_EJECT_TARGET,
    ID_SAFELY_REMOVE,
    ID_DB_OPEN,
    ID_DB_CREATE,
    ID_DB_CLOSE,
    ID_DB_INTEGRITY,
    ID_DB_BROWSER,
    ID_DB_ADMIN,
    ID_DB_USER_MOUNTS,
    ID_VIEW_LOG,
    ID_EDIT_RAW_DISK,
    ID_EDIT_ON_FILES,
    ID_EDIT_DRY_RUN,
    ID_EDIT_WT_THROUGH,
    ID_EDIT_WT_CACHESTAT,
    ID_EDIT_WT_MINCORE,
    ID_EDIT_VERIFY_WRITES,
    ID_LOG_ERROR,
    ID_LOG_WARN,
    ID_LOG_INFO,
    ID_LOG_DEBUG,
    ID_LOG_TRACE,
    ID_HELP_USAGE,
    ID_START,
    ID_PAUSE,
    ID_STOP,
    ID_DEVICE_REFRESH,
    ID_DEVICE_BASE = wxID_HIGHEST + 200,
    ID_DEVICE_MAX = wxID_HIGHEST + 699,
    ID_RECENT_BASE = wxID_HIGHEST + 700,
    ID_RECENT_MAX = wxID_HIGHEST + 799,
};

wxString format_rate(double bps) {
    if (bps < 1024)
        return wxString::Format("%.0f B/s", bps);
    if (bps < 1024 * 1024)
        return wxString::Format("%.1f KiB/s", bps / 1024.0);
    return wxString::Format("%.2f MiB/s", bps / (1024.0 * 1024.0));
}

wxString format_eta(double sec) {
    if (sec < 0)
        return "--";
    int s = static_cast<int>(sec);
    int h = s / 3600;
    int m = (s % 3600) / 60;
    s %= 60;
    if (h > 0)
        return wxString::Format("%d:%02d:%02d", h, m, s);
    return wxString::Format("%d:%02d", m, s);
}

enum StatusField {
    SF_MSG = 0,
    SF_PHASE,
    SF_PCT,
    SF_RATE,
    SF_ETA,
    SF_CELLS,
    SF_DB,
    SF_ICON,
    SF_COUNT
};

enum WorkState { WORK_IDLE = 0, WORK_RUN, WORK_PAUSE, WORK_OK, WORK_ERR };


wxMenuItem *append_icon_item(wxMenu *menu, int id, const wxString &label, const wxArtID &art,
                             wxItemKind kind = wxITEM_NORMAL) {
    auto *item = new wxMenuItem(menu, id, label, wxEmptyString, kind);
    item->SetBitmap(wxArtProvider::GetBitmap(art, wxART_MENU));
    menu->Append(item);
    return item;
}

void enable_menu(wxMenuBar *mb, int id, bool on) {
    if (mb && mb->FindItem(id))
        mb->Enable(id, on);
}

wxBitmap work_bitmap(int state) {
    const char *art = wxART_INFORMATION;
    switch (state) {
    case WORK_RUN:
        art = wxART_GO_FORWARD;
        break;
    case WORK_PAUSE:
        art = wxART_WARNING;
        break;
    case WORK_OK:
        art = wxART_TICK_MARK;
        break;
    case WORK_ERR:
        art = wxART_ERROR;
        break;
    default:
        art = wxART_INFORMATION;
        break;
    }
    return wxArtProvider::GetBitmap(art, wxART_MENU, wxSize(16, 16));
}

bool fstype_supported(const std::string &fs) {
    std::string t = fs;
    for (char &c : t)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "vfat" || t == "fat" || t == "fat12" || t == "fat16" || t == "fat32" ||
           t == "exfat" || t == "ntfs" || t == "ntfs3" || t == "ext2" || t == "ext3" || t == "ext4";
}

} /* namespace */

static const char *log_level_tag(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

static void mirror_log_stdout(const LogEntry &e) {
    std::fprintf(stdout, "%s: %s\n", log_level_tag(e.level), e.message.c_str());
    std::fflush(stdout);
}

MainFrame::MainFrame(Options opts)
    : wxFrame(nullptr, wxID_ANY, "reflash", wxDefaultPosition, wxSize(960, 640)),
      opts_(std::move(opts)), timer_(this) {
    const bool cli_target = !opts_.target.empty();
    db_cli_explicit_ = opts_.sqlite_db_explicit;
    const bool cli_db = db_cli_explicit_;
    restore_gui_session(opts_, cli_target, cli_db);

    build_menubar();
    build_toolbar();
    build_statusbar();

    auto *panel = new wxPanel(this);
    auto *root = new wxBoxSizer(wxVERTICAL);

    gauge_ = new wxGauge(panel, wxID_ANY, 1000);
    grid_ = new GridPanel(panel);
    log_ = new LogPanel(panel);

    root->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    root->Add(grid_, 1, wxEXPAND | wxALL, 6);
    root->Add(log_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    panel->SetSizer(root);

    auto *frame_sz = new wxBoxSizer(wxVERTICAL);
    frame_sz->Add(panel, 1, wxEXPAND);
    SetSizer(frame_sz);

    Bind(wxEVT_TIMER, &MainFrame::on_timer, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &ev) {
        save_session();
        ev.Skip();
    });

    update_title();
    set_field(SF_MSG, "Ready");
    if (log_)
        log_->set_min_level(LogLevel::Info);
    set_running_ui(false);
    if (!opts_.target.empty()) {
        refresh_target_fs("");
        remember_recent_target(opts_.target);
        update_title();
        maybe_open_builtin_manifest();
        if (cli_target)
            start_run();
        else
            set_status(wxString::Format("Target: %s", opts_.target));
    }
    if (!db_open_ && !opts_.sqlite_db.empty())
        open_database(opts_.sqlite_db);
    update_drive_ui();
    update_db_ui();
}

void MainFrame::build_menubar() {
    auto *menubar = new wxMenuBar();

    auto *file = new wxMenu();
    file_disks_ = new wxMenu();
    file_loops_ = new wxMenu();
    file_recents_ = new wxMenu();
    file->AppendSubMenu(file_disks_, "&Disks");
    file->AppendSeparator();
    file->AppendSubMenu(file_loops_, "&Loop");
    file->AppendSeparator();
    append_icon_item(file, ID_IMAGE_FILE, "&Image File...\tCtrl+O", wxART_FILE_OPEN);
    file->AppendSubMenu(file_recents_, "&Recents");
    file->AppendSeparator();
    append_icon_item(file, ID_CLOSE_TARGET, "&Close\tCtrl+W", wxART_CLOSE);
    append_icon_item(file, ID_EJECT_TARGET, "&Eject\tCtrl+E", wxART_CDROM);
    append_icon_item(file, ID_SAFELY_REMOVE, "&Safely Remove\tCtrl+Shift+E", wxART_QUIT);
    file->AppendSeparator();
    append_icon_item(file, wxID_EXIT, "&Quit\tCtrl+Q", wxART_QUIT);
    menubar->Append(file, "&File");

    auto *edit = new wxMenu();
    edit->AppendRadioItem(ID_EDIT_RAW_DISK, "&Raw Mode (entire disk)\tCtrl+1");
    edit->AppendRadioItem(ID_EDIT_ON_FILES, "&Files Mode (for supported fs)\tCtrl+2");
    if (opts_.mode == RunMode::Recursive)
        edit->Check(ID_EDIT_ON_FILES, true);
    else
        edit->Check(ID_EDIT_RAW_DISK, true);
    edit->AppendCheckItem(ID_EDIT_DRY_RUN, "&Dry-run\tCtrl+3");
    edit->Check(ID_EDIT_DRY_RUN, opts_.dry_run);
    edit->AppendSeparator();
    edit->AppendRadioItem(ID_EDIT_WT_THROUGH, "&Write through\tCtrl+4");
    edit->AppendRadioItem(ID_EDIT_WT_CACHESTAT, "Flush via &cachestat\tCtrl+5");
    edit->AppendRadioItem(ID_EDIT_WT_MINCORE, "Flush via &mincore\tCtrl+6");
    if (opts_.write_cache == WriteCacheMode::WriteThrough)
        edit->Check(ID_EDIT_WT_THROUGH, true);
    else if (opts_.write_cache == WriteCacheMode::Mincore)
        edit->Check(ID_EDIT_WT_MINCORE, true);
    else
        edit->Check(ID_EDIT_WT_CACHESTAT, true);
    edit->AppendCheckItem(ID_EDIT_VERIFY_WRITES, "&Verify writes\tCtrl+7");
    edit->Check(ID_EDIT_VERIFY_WRITES, opts_.verify_writes);
    edit->AppendSeparator();
    append_icon_item(edit, ID_START, "&Start\tCtrl+R", wxART_GO_FORWARD);
    append_icon_item(edit, ID_PAUSE, "&Pause\tCtrl+P", wxART_WARNING);
    append_icon_item(edit, ID_STOP, "S&top\tCtrl+.", wxART_CROSS_MARK);
    edit->AppendSeparator();
    append_icon_item(edit, wxID_PREFERENCES, "&Preferences...\tCtrl+,", wxART_EXECUTABLE_FILE);
    menubar->Append(edit, "&Edit");

    auto *db = new wxMenu();
    append_icon_item(db, ID_DB_CREATE, "&Create default\tCtrl+Shift+D", wxART_NEW);
    append_icon_item(db, ID_DB_OPEN, "&Use external SQLite...\tCtrl+D", wxART_FILE_OPEN);
    append_icon_item(db, ID_DB_CLOSE, "&Close\tCtrl+Shift+U", wxART_CLOSE);
    db->AppendSeparator();
    append_icon_item(db, ID_DB_USER_MOUNTS, "&User mounts support", wxART_TIP);
    append_icon_item(db, ID_DB_INTEGRITY, "&Validate\tCtrl+I", wxART_REPORT_VIEW);
    append_icon_item(db, ID_DB_BROWSER, "&Browse\tCtrl+B", wxART_FOLDER_OPEN);
    db->AppendSeparator();
    append_icon_item(db, ID_DB_ADMIN, "SQLite &admin...\tCtrl+Shift+A", wxART_REPORT_VIEW);
    menubar->Append(db, "&Manifest");

    auto *view = new wxMenu();
    view->AppendCheckItem(ID_VIEW_LOG, "Show &log\tCtrl+L");
    view->Check(ID_VIEW_LOG, true);
    auto *levels = new wxMenu();
    levels->AppendRadioItem(ID_LOG_ERROR, "&Error");
    levels->AppendRadioItem(ID_LOG_WARN, "&Warnings");
    levels->AppendRadioItem(ID_LOG_INFO, "&Information");
    levels->AppendRadioItem(ID_LOG_DEBUG, "&Debug");
    levels->AppendRadioItem(ID_LOG_TRACE, "&Trace");
    levels->Check(ID_LOG_INFO, true);
    view->AppendSubMenu(levels, "Logging &level");
    menubar->Append(view, "&View");

    auto *help = new wxMenu();
    append_icon_item(help, ID_HELP_USAGE, "&Usage\tF1", wxART_HELP);
    append_icon_item(help, wxID_ABOUT, "&About\tShift+F1", wxART_INFORMATION);
    menubar->Append(help, "&Help");

    SetMenuBar(menubar);
    rebuild_drive_menus();

    Bind(wxEVT_MENU, &MainFrame::on_image_file, this, ID_IMAGE_FILE);
    Bind(wxEVT_MENU, &MainFrame::on_close_target, this, ID_CLOSE_TARGET);
    Bind(wxEVT_MENU, &MainFrame::on_eject_target, this, ID_EJECT_TARGET);
    Bind(wxEVT_MENU, &MainFrame::on_safely_remove, this, ID_SAFELY_REMOVE);
    Bind(wxEVT_MENU, &MainFrame::on_quit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::on_db_create_default, this, ID_DB_CREATE);
    Bind(wxEVT_MENU, &MainFrame::on_db_open, this, ID_DB_OPEN);
    Bind(wxEVT_MENU, &MainFrame::on_db_close, this, ID_DB_CLOSE);
    Bind(wxEVT_MENU, &MainFrame::on_db_integrity, this, ID_DB_INTEGRITY);
    Bind(wxEVT_MENU, &MainFrame::on_db_browser, this, ID_DB_BROWSER);
    Bind(wxEVT_MENU, &MainFrame::on_db_admin, this, ID_DB_ADMIN);
    Bind(wxEVT_MENU, &MainFrame::on_db_user_mounts, this, ID_DB_USER_MOUNTS);
    Bind(wxEVT_MENU, &MainFrame::on_view_log, this, ID_VIEW_LOG);
    Bind(wxEVT_MENU, &MainFrame::on_edit_raw_disk, this, ID_EDIT_RAW_DISK);
    Bind(wxEVT_MENU, &MainFrame::on_edit_on_files, this, ID_EDIT_ON_FILES);
    Bind(wxEVT_MENU, &MainFrame::on_edit_dry_run, this, ID_EDIT_DRY_RUN);
    Bind(wxEVT_MENU, &MainFrame::on_edit_write_cache, this, ID_EDIT_WT_THROUGH);
    Bind(wxEVT_MENU, &MainFrame::on_edit_write_cache, this, ID_EDIT_WT_CACHESTAT);
    Bind(wxEVT_MENU, &MainFrame::on_edit_write_cache, this, ID_EDIT_WT_MINCORE);
    Bind(wxEVT_MENU, &MainFrame::on_edit_verify_writes, this, ID_EDIT_VERIFY_WRITES);
    Bind(wxEVT_MENU, &MainFrame::on_log_level, this, ID_LOG_ERROR);
    Bind(wxEVT_MENU, &MainFrame::on_log_level, this, ID_LOG_WARN);
    Bind(wxEVT_MENU, &MainFrame::on_log_level, this, ID_LOG_INFO);
    Bind(wxEVT_MENU, &MainFrame::on_log_level, this, ID_LOG_DEBUG);
    Bind(wxEVT_MENU, &MainFrame::on_log_level, this, ID_LOG_TRACE);
    Bind(wxEVT_MENU, &MainFrame::on_edit_prefs, this, wxID_PREFERENCES);
    Bind(wxEVT_MENU, &MainFrame::on_help_usage, this, ID_HELP_USAGE);
    Bind(wxEVT_MENU, &MainFrame::on_help_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_start, this, ID_START);
    Bind(wxEVT_MENU, &MainFrame::on_pause, this, ID_PAUSE);
    Bind(wxEVT_MENU, &MainFrame::on_stop, this, ID_STOP);
    Bind(wxEVT_MENU, &MainFrame::on_device_pick, this, ID_DEVICE_BASE, ID_DEVICE_MAX);
    Bind(wxEVT_MENU, &MainFrame::on_recent_pick, this, ID_RECENT_BASE, ID_RECENT_MAX);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { rebuild_drive_menus(); }, ID_DEVICE_REFRESH);
}

void MainFrame::build_toolbar() {
    toolbar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_TEXT);
    toolbar_->AddTool(ID_DRIVE, "Drive", wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_TOOLBAR),
                      wxNullBitmap, wxITEM_CHECK);
    toolbar_->AddTool(ID_EJECT_TARGET, "Eject", wxArtProvider::GetBitmap(wxART_CDROM, wxART_TOOLBAR));
    toolbar_->AddTool(ID_SAFELY_REMOVE, "Safely Remove",
                      wxArtProvider::GetBitmap(wxART_QUIT, wxART_TOOLBAR));
    toolbar_->AddSeparator();
    toolbar_->AddTool(ID_MANIFEST, "Manifest", wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_TOOLBAR),
                      wxNullBitmap, wxITEM_CHECK);
    toolbar_->AddSeparator();
    toolbar_->AddTool(ID_START, "Start", wxArtProvider::GetBitmap(wxART_GO_FORWARD, wxART_TOOLBAR));
    toolbar_->AddTool(ID_PAUSE, "Pause", wxArtProvider::GetBitmap(wxART_WARNING, wxART_TOOLBAR));
    toolbar_->AddTool(ID_STOP, "Stop", wxArtProvider::GetBitmap(wxART_CROSS_MARK, wxART_TOOLBAR));
    toolbar_->AddSeparator();
    toolbar_->AddTool(ID_DB_INTEGRITY, "Validate",
                      wxArtProvider::GetBitmap(wxART_REPORT_VIEW, wxART_TOOLBAR));
    toolbar_->AddTool(ID_DB_BROWSER, "Browse",
                      wxArtProvider::GetBitmap(wxART_FOLDER_OPEN, wxART_TOOLBAR));
    toolbar_->AddSeparator();
    toolbar_->AddTool(wxID_ABOUT, "About", wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_TOOLBAR));
    toolbar_->Realize();

    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_drive_tool, this, ID_DRIVE);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_eject_target, this, ID_EJECT_TARGET);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_safely_remove, this, ID_SAFELY_REMOVE);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_manifest_tool, this, ID_MANIFEST);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_start, this, ID_START);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_pause, this, ID_PAUSE);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_stop, this, ID_STOP);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_db_integrity, this, ID_DB_INTEGRITY);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_db_browser, this, ID_DB_BROWSER);
    toolbar_->Bind(wxEVT_TOOL, &MainFrame::on_help_about, this, wxID_ABOUT);
}

void MainFrame::build_statusbar() {
    auto *bar = new wxStatusBar(this);
    SetStatusBar(bar);
    /* Extra width for per-field icons (phase/pct/rate/eta/cells/db). */
    static const int widths[SF_COUNT] = {-1, 150, 80, 120, 100, 140, 190, 28};
    bar->SetFieldsCount(SF_COUNT);
    bar->SetStatusWidths(SF_COUNT, widths);

    field_icons_[SF_PHASE] = make_field_icon(bar, wxART_GO_FORWARD);
    field_icons_[SF_PCT] = make_field_icon(bar, wxART_LIST_VIEW);
    field_icons_[SF_RATE] = make_field_icon(bar, wxART_GO_UP);
    field_icons_[SF_ETA] = make_field_icon(bar, wxART_TIP);
    field_icons_[SF_CELLS] = make_field_icon(bar, wxART_REPORT_VIEW);
    field_icons_[SF_DB] = make_field_icon(bar, wxART_NORMAL_FILE);

    work_icon_ = new wxStaticBitmap(bar, wxID_ANY, work_bitmap(WORK_IDLE));
    bar->Bind(wxEVT_SIZE, [this](wxSizeEvent &ev) {
        place_field_icons();
        place_work_icon();
        ev.Skip();
    });
    set_field(SF_DB, "No database");
    CallAfter([this] {
        place_field_icons();
        place_work_icon();
    });
}

wxStaticBitmap *MainFrame::make_field_icon(wxStatusBar *bar, const char *art) {
    wxBitmap bmp = wxArtProvider::GetBitmap(art, wxART_MENU, wxSize(14, 14));
    auto *ico = new wxStaticBitmap(bar, wxID_ANY, bmp);
    ico->Hide();
    return ico;
}

void MainFrame::place_field_icons() {
    auto *bar = GetStatusBar();
    if (!bar)
        return;
    for (int i = 0; i < SF_ICON; ++i) {
        wxStaticBitmap *ico = field_icons_[i];
        if (!ico)
            continue;
        wxRect r;
        if (!bar->GetFieldRect(i, r) || r.width < 8) {
            ico->Hide();
            continue;
        }
        wxSize sz = ico->GetSize();
        int x = r.x + 3;
        int y = r.y + (r.height - sz.y) / 2;
        ico->SetSize(x, y, sz.x, sz.y);
        ico->Show();
    }
}

void MainFrame::place_work_icon() {
    auto *bar = GetStatusBar();
    if (!bar || !work_icon_)
        return;
    wxRect r;
    if (!bar->GetFieldRect(SF_ICON, r))
        return;
    wxSize sz = work_icon_->GetSize();
    work_icon_->Move(r.x + (r.width - sz.x) / 2, r.y + (r.height - sz.y) / 2);
}

void MainFrame::set_work_state(int state) {
    if (work_icon_)
        work_icon_->SetBitmap(work_bitmap(state));
    place_work_icon();
}

void MainFrame::fill_drive_menu(wxMenu *disks, wxMenu *loops, wxMenu *recents) {
    device_list_.clear();
    auto clear_menu = [](wxMenu *m) {
        while (m && m->GetMenuItemCount() > 0)
            m->Destroy(m->FindItemByPosition(0));
    };
    clear_menu(disks);
    clear_menu(loops);
    clear_menu(recents);

    auto append_dev = [&](wxMenu *menu, const BlockDevInfo &d, int &id) {
        if (id > ID_DEVICE_MAX)
            return;
        wxString label = d.path;
        if (!d.label.empty())
            label += "   " + d.label;
        if (!d.fstype.empty())
            label += "   " + d.fstype;
        if (!d.size.empty())
            label += "   " + d.size;
        if (!d.mountpoint.empty())
            label += "   [" + d.mountpoint + "]";
        menu->Append(id, label);
        device_list_.push_back(d);
        ++id;
    };

    int id = ID_DEVICE_BASE;
    bool any_disk = false;
    bool any_loop = false;
    for (const auto &d : list_block_devices()) {
        std::string g = device_group_label(d);
        if (g == "Loop") {
            append_dev(loops, d, id);
            any_loop = true;
        } else {
            append_dev(disks, d, id);
            any_disk = true;
        }
    }
    if (!any_disk)
        disks->Append(wxID_ANY, "(no disks)")->Enable(false);
    else {
        disks->AppendSeparator();
        disks->Append(ID_DEVICE_REFRESH, "&Refresh list\tF5");
    }
    if (!any_loop)
        loops->Append(wxID_ANY, "(no loop devices)")->Enable(false);

    auto recent = load_recent_targets();
    int rid = ID_RECENT_BASE;
    if (recent.empty()) {
        recents->Append(wxID_ANY, "(empty)")->Enable(false);
    } else {
        for (const auto &p : recent) {
            if (rid > ID_RECENT_MAX)
                break;
            recents->Append(rid++, wxString::FromUTF8(p));
        }
    }
}

void MainFrame::rebuild_drive_menus() {
    fill_drive_menu(file_disks_, file_loops_, file_recents_);
}

void MainFrame::popup_drive_menu() {
    wxMenu popup;
    auto *disks = new wxMenu();
    auto *loops = new wxMenu();
    auto *recents = new wxMenu();
    fill_drive_menu(disks, loops, recents);
    popup.AppendSubMenu(disks, "Disks");
    popup.AppendSeparator();
    popup.AppendSubMenu(loops, "Loop");
    popup.AppendSeparator();
    popup.Append(ID_IMAGE_FILE, "Image File...");
    popup.AppendSubMenu(recents, "Recents");
    PopupMenu(&popup);
}

void MainFrame::update_title() {
    wxString t = "reflash";
    if (!opts_.target.empty())
        t += " - " + opts_.target;
    if (!opts_.sqlite_db.empty())
        t += " [" + opts_.sqlite_db + "]";
    SetTitle(t);
}

void MainFrame::set_field(int index, const wxString &text) {
    if (index < 0 || index >= SF_ICON)
        return;
    wxString shown = text;
    /* Leave room for the field icon so text is not drawn underneath. */
    if (field_icons_[index] && !text.empty())
        shown = "    " + text;
    if (auto *bar = GetStatusBar())
        bar->SetStatusText(shown, index);
}

void MainFrame::set_status(const wxString &text) { set_field(SF_MSG, text); }

void MainFrame::refresh_db_status() {
    if (!db_open_) {
        set_field(SF_DB, "No database");
        return;
    }
    wxString name = catalog_.path();
    auto slash = name.Find('/', true);
    if (slash != wxNOT_FOUND && slash + 1 < static_cast<int>(name.length()))
        name = name.Mid(slash + 1);
    set_field(SF_DB, name);
}

void MainFrame::refresh_target_fs(const std::string &fstype_hint) {
    target_fs_ok_ = false;
    target_fstype_.clear();
    if (fstype_supported(fstype_hint)) {
        target_fstype_ = fstype_hint;
        target_fs_ok_ = true;
        return;
    }
    if (opts_.target.empty())
        return;
    Device dev;
    if (!dev.open(opts_.target)) {
        target_fstype_ = fstype_hint;
        target_fs_ok_ = fstype_supported(fstype_hint);
        return;
    }
    std::string err;
    FsType t = detect_fs(dev, 0, &err);
    dev.close();
    if (t != FsType::Unknown) {
        target_fstype_ = fs_type_name(t);
        target_fs_ok_ = true;
    } else if (fstype_supported(fstype_hint)) {
        target_fstype_ = fstype_hint;
        target_fs_ok_ = true;
    } else {
        target_fstype_ = fstype_hint;
    }
}

void MainFrame::update_drive_ui() {
    const bool has = !opts_.target.empty();
    const bool busy = engine_ && engine_->running();
    if (toolbar_ && toolbar_->FindById(ID_DRIVE))
        toolbar_->ToggleTool(ID_DRIVE, has);
    auto *mb = GetMenuBar();
    if (mb) {
        enable_menu(mb, ID_CLOSE_TARGET, has && !busy);
        enable_menu(mb, ID_EJECT_TARGET, has && !busy);
        enable_menu(mb, ID_SAFELY_REMOVE, has && !busy);
    }
    if (toolbar_) {
        if (toolbar_->FindById(ID_EJECT_TARGET))
            toolbar_->EnableTool(ID_EJECT_TARGET, has && !busy);
        if (toolbar_->FindById(ID_SAFELY_REMOVE))
            toolbar_->EnableTool(ID_SAFELY_REMOVE, has && !busy);
    }
}

void MainFrame::update_db_ui() {
    const bool mounts_ok = user_mounts_ready();
    /* Validate only needs an open catalog; Browse needs mounts (+ db, auto-opened). */
    const bool can_validate = db_open_;
    const bool can_browse = mounts_ok && (db_open_ || !opts_.target.empty());
    auto *mb = GetMenuBar();
    if (mb) {
        enable_menu(mb, ID_DB_CLOSE, db_open_);
        enable_menu(mb, ID_DB_INTEGRITY, can_validate);
        enable_menu(mb, ID_DB_BROWSER, can_browse);
        enable_menu(mb, ID_DB_ADMIN, db_open_);
        enable_menu(mb, ID_DB_CREATE, !opts_.target.empty());
        enable_menu(mb, ID_DB_OPEN, true);
        enable_menu(mb, ID_DB_USER_MOUNTS, true);
    }
    if (toolbar_) {
        if (toolbar_->FindById(ID_MANIFEST))
            toolbar_->ToggleTool(ID_MANIFEST, db_open_);
        if (toolbar_->FindById(ID_DB_INTEGRITY))
            toolbar_->EnableTool(ID_DB_INTEGRITY, can_validate);
        if (toolbar_->FindById(ID_DB_BROWSER))
            toolbar_->EnableTool(ID_DB_BROWSER, can_browse);
    }
    refresh_db_status();
}

void MainFrame::open_database(const std::string &path) {
    if (path.empty())
        return;
    {
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            std::string dir = path.substr(0, slash);
            ::mkdir(dir.c_str(), 0755);
        }
    }
    std::string err;
    if (!catalog_.open(path, &err)) {
        alert_box(err, "reflash", wxOK | wxICON_ERROR, this);
        update_db_ui();
        return;
    }
    opts_.sqlite_db = path;
    opts_.sha1_valid_days = catalog_.sha1_valid_days();
    db_open_ = true;
    update_title();
    update_db_ui();
    save_session();
}

void MainFrame::close_database() {
    if (!db_open_)
        return;
    if (browser_) {
        browser_->Close(true);
        browser_ = nullptr;
    }
    catalog_.close();
    db_open_ = false;
    opts_.sqlite_db.clear();
    update_title();
    update_db_ui();
    save_session();
}

void MainFrame::maybe_open_builtin_manifest() {
    if (db_cli_explicit_ || opts_.target.empty())
        return;
    std::string builtin = find_builtin_manifest_db(opts_.target);
    if (builtin.empty())
        return;
    if (db_open_ && opts_.sqlite_db == builtin)
        return;
    open_database(builtin);
    set_status(wxString::Format("Built-in manifest: %s", builtin));
}

void MainFrame::set_pause_label(bool is_paused) {
    if (auto *mb = GetMenuBar()) {
        if (auto *item = mb->FindItem(ID_PAUSE))
            item->SetItemLabel(is_paused ? "&Resume\tCtrl+P" : "&Pause\tCtrl+P");
    }
    if (toolbar_) {
        if (auto *tool = toolbar_->FindById(ID_PAUSE))
            tool->SetLabel(is_paused ? "Resume" : "Pause");
        toolbar_->Realize();
    }
}

void MainFrame::set_running_ui(bool running) {
    const bool files_blocked = opts_.mode == RunMode::Recursive && !target_fs_ok_;
    const bool can_start = !running && !opts_.target.empty() && !files_blocked;
    if (!running)
        set_pause_label(false);
    auto *mb = GetMenuBar();
    if (mb) {
        enable_menu(mb, ID_IMAGE_FILE, !running);
        enable_menu(mb, ID_CLOSE_TARGET, !running && !opts_.target.empty());
        enable_menu(mb, ID_EJECT_TARGET, !running && !opts_.target.empty());
        enable_menu(mb, ID_SAFELY_REMOVE, !running && !opts_.target.empty());
        enable_menu(mb, ID_EDIT_RAW_DISK, !running);
        enable_menu(mb, ID_EDIT_ON_FILES, !running);
        enable_menu(mb, ID_EDIT_DRY_RUN, !running);
        enable_menu(mb, ID_EDIT_WT_THROUGH, !running);
        enable_menu(mb, ID_EDIT_WT_CACHESTAT, !running);
        enable_menu(mb, ID_EDIT_WT_MINCORE, !running);
        enable_menu(mb, ID_EDIT_VERIFY_WRITES, !running);
        enable_menu(mb, wxID_PREFERENCES, !running);
        enable_menu(mb, ID_START, can_start);
        enable_menu(mb, ID_PAUSE, running);
        enable_menu(mb, ID_STOP, running);
        enable_menu(mb, ID_DB_CREATE, !running && !opts_.target.empty());
        enable_menu(mb, ID_DB_OPEN, !running);
        enable_menu(mb, ID_DB_CLOSE, !running && db_open_);
        enable_menu(mb, ID_DB_INTEGRITY, !running && db_open_);
        enable_menu(mb, ID_DB_BROWSER,
                    !running && user_mounts_ready() && (db_open_ || !opts_.target.empty()));
        enable_menu(mb, ID_DB_ADMIN, !running && db_open_);
        enable_menu(mb, ID_DB_USER_MOUNTS, !running);
    }
    if (toolbar_) {
        toolbar_->EnableTool(ID_DRIVE, !running);
        if (toolbar_->FindById(ID_EJECT_TARGET))
            toolbar_->EnableTool(ID_EJECT_TARGET, !running && !opts_.target.empty());
        if (toolbar_->FindById(ID_SAFELY_REMOVE))
            toolbar_->EnableTool(ID_SAFELY_REMOVE, !running && !opts_.target.empty());
        toolbar_->EnableTool(ID_MANIFEST, !running);
        if (toolbar_->FindById(ID_START))
            toolbar_->EnableTool(ID_START, can_start);
        if (toolbar_->FindById(ID_PAUSE))
            toolbar_->EnableTool(ID_PAUSE, running);
        if (toolbar_->FindById(ID_STOP))
            toolbar_->EnableTool(ID_STOP, running);
        if (toolbar_->FindById(ID_DB_BROWSER))
            toolbar_->EnableTool(ID_DB_BROWSER, !running && user_mounts_ready() &&
                                              (db_open_ || !opts_.target.empty()));
        if (toolbar_->FindById(ID_DB_INTEGRITY))
            toolbar_->EnableTool(ID_DB_INTEGRITY, !running && db_open_);
    }
    update_drive_ui();
    update_db_ui();
    if (running)
        set_work_state(paused_ ? WORK_PAUSE : WORK_RUN);
    else if (!files_blocked)
        set_work_state(WORK_IDLE);
    if (!running && files_blocked && !opts_.target.empty())
        set_status("Files mode: filesystem not supported");
}

bool MainFrame::ensure_engine() {
    if (!engine_)
        engine_ = std::make_shared<MassageEngine>(opts_);
    else
        engine_->set_options(opts_);
    return true;
}

void MainFrame::start_run() {
    if (opts_.target.empty()) {
        alert_box("Open a file or choose a device first.", "reflash", wxOK | wxICON_INFORMATION,
                     this);
        return;
    }
    if (engine_ && engine_->running())
        return;
    if (engine_)
        engine_->join();
    ensure_engine();
    offered_bad_ = false;
    paused_ = false;
    set_pause_label(false);
    if (log_)
        log_->clear();
    /* Show the old map as pending before the worker paints over it. */
    if (grid_)
        grid_->reset_pending();
    if (gauge_)
        gauge_->SetValue(0);
    ++grid_resets_;
    std::string err;
    if (!engine_->start(&err)) {
        alert_box(err, "reflash", wxOK | wxICON_ERROR, this);
        set_running_ui(false);
        return;
    }
    set_running_ui(true);
    set_status("Starting...");
    timer_.Start(50);
}

void MainFrame::open_target(const std::string &path, const std::string &fstype_hint) {
    if (browser_ && path != opts_.target) {
        browser_->Close(true);
        browser_ = nullptr;
    }
    opts_.target = path;
    refresh_target_fs(fstype_hint);
    remember_recent_target(path);
    rebuild_drive_menus();
    update_title();
    maybe_open_builtin_manifest();
    set_status(wxString::Format("Target: %s", opts_.target));
    set_running_ui(false);
    save_session();
}

void MainFrame::close_target() {
    if (engine_ && engine_->running())
        return;
    if (browser_) {
        browser_->Close(true);
        browser_ = nullptr;
    }
    /* Manifest on the drive must be closed before the drive goes away. */
    if (manifest_on_target())
        close_database();
    opts_.target.clear();
    target_fs_ok_ = false;
    target_fstype_.clear();
    if (grid_)
        grid_->clear();
    if (gauge_)
        gauge_->SetValue(0);
    set_field(SF_PHASE, "");
    set_field(SF_PCT, "");
    set_field(SF_RATE, "");
    set_field(SF_ETA, "");
    set_field(SF_CELLS, "");
    update_title();
    set_status("No target");
    set_running_ui(false);
    save_session();
}

bool MainFrame::manifest_on_target() const {
    if (!db_open_ || opts_.sqlite_db.empty() || opts_.target.empty())
        return false;

    auto under = [](const std::string &path, const std::string &root) {
        if (root.empty() || path.empty())
            return false;
        char rp[PATH_MAX], rr[PATH_MAX];
        std::string p = path, r = root;
        if (::realpath(path.c_str(), rp))
            p = rp;
        if (::realpath(root.c_str(), rr))
            r = rr;
        if (p.size() < r.size())
            return false;
        if (p.compare(0, r.size(), r) != 0)
            return false;
        return p.size() == r.size() || p[r.size()] == '/';
    };

    if (path_is_directory(opts_.target) && under(opts_.sqlite_db, opts_.target))
        return true;

    std::string builtin = find_builtin_manifest_db(opts_.target);
    if (!builtin.empty()) {
        char a[PATH_MAX], b[PATH_MAX];
        std::string db = opts_.sqlite_db, bi = builtin;
        if (::realpath(db.c_str(), a))
            db = a;
        if (::realpath(bi.c_str(), b))
            bi = b;
        if (db == bi)
            return true;
    }

    for (const auto &m : find_mounts_for_device(opts_.target)) {
        if (!m.target.empty() && under(opts_.sqlite_db, m.target))
            return true;
    }
    return false;
}

void MainFrame::eject_or_remove(bool power_off) {
    if (engine_ && engine_->running())
        return;
    if (opts_.target.empty())
        return;

    const std::string target = opts_.target;
    if (browser_) {
        browser_->Close(true);
        browser_ = nullptr;
    }
    if (manifest_on_target())
        close_database();

    std::string err;
    bool ok = power_off ? safely_remove_device(target, &err) : eject_device(target, &err);
    /* Both actions imply Close — clear the drive from the UI either way once
     * we attempted removal (close_target is a no-op if already cleared). */
    close_target();
    if (!ok) {
        alert_box(err.empty() ? (power_off ? "Safely remove failed." : "Eject failed.") : err,
                  "reflash", wxOK | wxICON_WARNING, this);
        set_status(power_off ? "Safely remove failed" : "Eject failed");
    } else {
        set_status(wxString::Format(power_off ? "Safely removed: %s" : "Ejected: %s", target));
    }
}

size_t MainFrame::grid_cells() const { return grid_ ? grid_->cell_count() : 0; }

size_t MainFrame::grid_status_count(CellStatus st) const {
    return grid_ ? grid_->count_status(st) : 0;
}

size_t MainFrame::log_count() const { return log_ ? log_->row_count() : 0; }

void MainFrame::save_session() const { save_gui_session(opts_); }

void MainFrame::on_drive_tool(wxCommandEvent &) {
    if (engine_ && engine_->running()) {
        if (toolbar_)
            toolbar_->ToggleTool(ID_DRIVE, !opts_.target.empty());
        return;
    }
    if (!opts_.target.empty()) {
        close_target();
        return;
    }
    /* Tool was just checked; show the menu, then sync the pressed state. */
    if (toolbar_)
        toolbar_->ToggleTool(ID_DRIVE, false);
    popup_drive_menu();
    rebuild_drive_menus();
    update_drive_ui();
}

void MainFrame::on_manifest_tool(wxCommandEvent &) {
    if (engine_ && engine_->running()) {
        if (toolbar_)
            toolbar_->ToggleTool(ID_MANIFEST, db_open_);
        return;
    }
    if (db_open_) {
        close_database();
        return;
    }
    if (toolbar_)
        toolbar_->ToggleTool(ID_MANIFEST, false);
    wxCommandEvent open_ev;
    on_db_open(open_ev);
    update_db_ui();
}

void MainFrame::on_image_file(wxCommandEvent &) {
    if (engine_ && engine_->running())
        return;
    wxFileDialog dlg(this, "Open image file", "", "",
                     "All files (*)|*|Disk images (*.img;*.iso;*.raw)|*.img;*.iso;*.raw",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    open_target(dlg.GetPath().ToStdString());
}

void MainFrame::on_close_target(wxCommandEvent &) { close_target(); }

void MainFrame::on_eject_target(wxCommandEvent &) { eject_or_remove(false); }

void MainFrame::on_safely_remove(wxCommandEvent &) { eject_or_remove(true); }

void MainFrame::on_quit(wxCommandEvent &) {
    if (engine_ && engine_->running()) {
        engine_->pause().request_stop();
        engine_->pause().resume();
        engine_->join();
    }
    Close(true);
}

void MainFrame::on_device_pick(wxCommandEvent &ev) {
    if (engine_ && engine_->running())
        return;
    int idx = ev.GetId() - ID_DEVICE_BASE;
    if (idx < 0 || static_cast<size_t>(idx) >= device_list_.size())
        return;
    const BlockDevInfo &d = device_list_[static_cast<size_t>(idx)];
    open_target(d.path, d.fstype);
}

void MainFrame::on_recent_pick(wxCommandEvent &ev) {
    if (engine_ && engine_->running())
        return;
    int idx = ev.GetId() - ID_RECENT_BASE;
    auto recent = load_recent_targets();
    if (idx < 0 || static_cast<size_t>(idx) >= recent.size())
        return;
    open_target(recent[static_cast<size_t>(idx)]);
}

void MainFrame::on_db_create_default(wxCommandEvent &) {
    if (engine_ && engine_->running())
        return;
    if (opts_.target.empty()) {
        alert_box("Open a drive (disk, loop, or image) first.", "reflash", wxOK | wxICON_INFORMATION,
                  this);
        return;
    }
    std::string path = find_builtin_manifest_db(opts_.target);
    if (path.empty()) {
        if (path_is_directory(opts_.target)) {
            path = opts_.target;
            if (!path.empty() && path.back() == '/')
                path.pop_back();
            path += "/manifest.db";
        } else {
            auto mounts = find_mounts_for_device(opts_.target);
            if (!mounts.empty()) {
                path = mounts.front().target;
                if (!path.empty() && path.back() == '/')
                    path.pop_back();
                path += "/manifest.db";
            } else {
                path = default_db_path(opts_.target);
            }
        }
    }
    open_database(path);
    if (db_open_)
        set_status(wxString::Format("Manifest: %s", path));
}

void MainFrame::on_db_open(wxCommandEvent &) {
    if (engine_ && engine_->running())
        return;
    wxFileDialog dlg(this, "Use external SQLite database", "", "",
                     "SQLite (*.sqlite;*.db)|*.sqlite;*.db|All files (*)|*", wxFD_OPEN);
    if (dlg.ShowModal() != wxID_OK) {
        update_db_ui();
        return;
    }
    open_database(dlg.GetPath().ToStdString());
}

void MainFrame::on_db_close(wxCommandEvent &) { close_database(); }

void MainFrame::on_db_integrity(wxCommandEvent &) {
    if (!db_open_) {
        alert_box("Open a manifest database first.", "reflash", wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxDialog dlg(this, wxID_ANY, "Validate manifest", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *root = new wxBoxSizer(wxVERTICAL);

    /* Plain strings kept so Wrap() can be redone when the dialog resizes. */
    struct WrapText {
        wxStaticText *ctrl = nullptr;
        wxString plain;
    };
    std::vector<WrapText> wraps;
    auto add_wrap = [&](wxStaticText *ctrl, const wxString &plain) {
        wraps.push_back({ctrl, plain});
    };

    auto *hdr = new wxBoxSizer(wxHORIZONTAL);
    hdr->Add(new wxStaticBitmap(&dlg, wxID_ANY,
                                wxArtProvider::GetBitmap(wxART_REPORT_VIEW, wxART_MESSAGE_BOX)),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    auto *hdr_txt = new wxBoxSizer(wxVERTICAL);
    hdr_txt->Add(new wxStaticText(&dlg, wxID_ANY, "Checking the manifest database"), 0, wxEXPAND);
    const wxString sub_plain =
        "Large catalogs can take a while. You can pause or cancel.";
    auto *sub = new wxStaticText(&dlg, wxID_ANY, sub_plain);
    add_wrap(sub, sub_plain);
    hdr_txt->Add(sub, 0, wxEXPAND | wxTOP, 4);
    hdr->Add(hdr_txt, 1, wxEXPAND);
    root->Add(hdr, 0, wxEXPAND | wxALL, 12);

    auto *status = new wxStaticText(&dlg, wxID_ANY, "Starting…");
    wraps.push_back({status, "Starting…"});
    root->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    auto *gauge = new wxGauge(&dlg, wxID_ANY, 1000);
    root->Add(gauge, 0, wxEXPAND | wxALL, 12);
    auto *eta = new wxStaticText(&dlg, wxID_ANY, "ETA: —");
    root->Add(eta, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    auto *log = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    root->Add(log, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *btn_validate = new wxButton(&dlg, wxID_ANY, "Validate");
    auto *btn_pause = new wxButton(&dlg, wxID_ANY, "Pause");
    auto *btn_cancel = new wxButton(&dlg, wxID_CANCEL, "Cancel");
    auto *btn_close = new wxButton(&dlg, wxID_OK, "Close");
    btn_validate->Enable(false);
    btn_close->Enable(false);
    btns->Add(btn_validate, 0, wxRIGHT, 8);
    btns->Add(btn_pause, 0, wxRIGHT, 8);
    btns->Add(btn_cancel, 0, wxRIGHT, 8);
    btns->AddStretchSpacer(1);
    btns->Add(btn_close, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, 12);
    dlg.SetSizer(root);
    dlg.SetMinSize(wxSize(360, 280));

    auto rewrap = [&]() {
        int w = dlg.GetClientSize().GetWidth() - 40;
        if (w < 120)
            w = 120;
        dlg.Freeze();
        for (auto &wt : wraps) {
            if (!wt.ctrl)
                continue;
            wt.ctrl->SetLabel(wt.plain);
            if (!wt.plain.empty())
                wt.ctrl->Wrap(w);
        }
        root->Layout();
        dlg.Thaw();
    };
    dlg.Bind(wxEVT_SIZE, [&](wxSizeEvent &ev) {
        rewrap();
        ev.Skip();
    });
    dlg.SetClientSize(520, 360);
    rewrap();

    auto pause_flag = std::make_shared<std::atomic<bool>>(false);
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto running = std::make_shared<std::atomic<bool>>(false);
    auto report_ptr = std::make_shared<std::string>();
    auto ok_ptr = std::make_shared<bool>(false);
    auto worker = std::make_shared<std::thread>();

    auto *timer = new wxTimer(&dlg);
    struct SharedSnap {
        std::mutex mu;
        Store::ValidateStatus st;
    };
    auto snap = std::make_shared<SharedSnap>();

    auto set_status = [&](const wxString &plain) {
        if (wraps.size() < 2)
            return;
        if (wraps[1].plain == plain)
            return;
        wraps[1].plain = plain;
        int w = dlg.GetClientSize().GetWidth() - 40;
        if (w < 120)
            w = 120;
        status->SetLabel(plain);
        if (!plain.empty())
            status->Wrap(w);
        root->Layout();
    };

    auto join_worker = [worker]() {
        if (worker->joinable())
            worker->join();
    };

    auto start_validate = [&]() {
        if (running->load())
            return;
        join_worker();
        pause_flag->store(false);
        cancel_flag->store(false);
        finished->store(false);
        running->store(true);
        *ok_ptr = false;
        report_ptr->clear();
        {
            std::lock_guard<std::mutex> lock(snap->mu);
            snap->st = {};
        }
        gauge->SetValue(0);
        eta->SetLabel("ETA: —");
        log->ChangeValue("");
        set_status("Starting…");
        btn_validate->Enable(false);
        btn_pause->Enable(true);
        btn_pause->SetLabel("Pause");
        btn_cancel->Enable(true);
        btn_close->Enable(false);

        *worker = std::thread([this, snap, pause_flag, cancel_flag, finished, running, report_ptr,
                               ok_ptr]() {
            std::string err;
            *ok_ptr = catalog_.validate_manifest(
                [snap](const Store::ValidateStatus &s) {
                    std::lock_guard<std::mutex> lock(snap->mu);
                    snap->st = s;
                },
                pause_flag.get(), cancel_flag.get(), report_ptr.get(), &err);
            if (!err.empty() && report_ptr->find(err) == std::string::npos) {
                if (!report_ptr->empty())
                    *report_ptr += "\n";
                *report_ptr += err;
            }
            finished->store(true);
            running->store(false);
        });
        timer->Start(200);
    };

    btn_pause->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        if (!running->load())
            return;
        bool now = !pause_flag->load();
        pause_flag->store(now);
        btn_pause->SetLabel(now ? "Resume" : "Pause");
        if (now)
            set_status("Paused");
    });
    btn_cancel->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        if (!running->load())
            return;
        cancel_flag->store(true);
        pause_flag->store(false);
        set_status("Cancelling…");
        btn_pause->Enable(false);
        btn_cancel->Enable(false);
    });

    dlg.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
        Store::ValidateStatus st;
        {
            std::lock_guard<std::mutex> lock(snap->mu);
            st = snap->st;
        }
        if ((!pause_flag->load() || finished->load()) && !st.phase.empty())
            set_status(wxString::FromUTF8(st.phase));
        if (st.total > 0) {
            int v = static_cast<int>((st.done * 1000) / st.total);
            gauge->SetValue(std::min(1000, v));
        }
        if (st.eta_seconds >= 0)
            eta->SetLabel(wxString::Format("ETA: %.0f s", st.eta_seconds));
        else
            eta->SetLabel("ETA: —");
        if (finished->load() && !running->load()) {
            timer->Stop();
            btn_pause->Enable(false);
            btn_cancel->Enable(false);
            btn_validate->Enable(true);
            btn_close->Enable(true);
            if (!report_ptr->empty())
                log->ChangeValue(wxString::FromUTF8(*report_ptr));
            if (*ok_ptr)
                gauge->SetValue(1000);
            finished->store(false);
        }
    });

    btn_validate->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { start_validate(); });

    start_validate();
    dlg.ShowModal();
    cancel_flag->store(true);
    pause_flag->store(false);
    join_worker();
    timer->Stop();
}

void MainFrame::on_db_browser(wxCommandEvent &) {
    if (!user_mounts_ready()) {
        alert_box("Browsing support is not set up yet.\n"
                  "Open Manifest → User mounts support to finish setup.",
                  "reflash", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (opts_.target.empty()) {
        alert_box("Open a drive (disk, loop, or image) first.", "reflash", wxOK | wxICON_INFORMATION,
                     this);
        return;
    }
    if (!db_open_) {
        maybe_open_builtin_manifest();
        if (!db_open_)
            open_database(default_db_path(opts_.target));
        if (!db_open_) {
            alert_box("Could not open or create a manifest database.", "reflash",
                         wxOK | wxICON_ERROR, this);
            return;
        }
    }
    if (browser_) {
        browser_->Raise();
        return;
    }

    std::string root;
    std::string device = opts_.target;
    std::vector<MountRecord> mounts;
    bool temp = false;
    std::string err;
    if (!mount_for_browse(opts_.target, &mounts, &temp, &err) || mounts.empty()) {
        alert_box(err.empty() ? "Could not open the drive for browsing." : err, "reflash",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    root = mounts.front().target;
    /* Directory browse: do not treat as a temp mount of a device image. */
    if (path_is_directory(opts_.target)) {
        device.clear();
        temp = false;
        mounts.clear();
    }

    browser_ = new BrowserFrame(this, &catalog_, root, device, std::move(mounts), temp);
    browser_->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &ev) {
        browser_ = nullptr;
        ev.Skip();
    });
    browser_->Show(true);
    set_status(wxString::Format("Manifest & Files: %s", opts_.target));
}

void MainFrame::on_view_log(wxCommandEvent &ev) {
    log_->Show(ev.IsChecked());
    Layout();
}

void MainFrame::on_edit_raw_disk(wxCommandEvent &) {
    opts_.mode = RunMode::Linear;
    set_status("Raw mode");
    set_running_ui(false);
    save_session();
}

void MainFrame::on_edit_dry_run(wxCommandEvent &ev) {
    opts_.dry_run = ev.IsChecked();
    set_status(opts_.dry_run ? "Dry-run (read only)" : "Dry-run off");
    save_session();
}

void MainFrame::on_edit_write_cache(wxCommandEvent &ev) {
    switch (ev.GetId()) {
    case ID_EDIT_WT_THROUGH:
        opts_.write_cache = WriteCacheMode::WriteThrough;
        set_status("Write through");
        break;
    case ID_EDIT_WT_MINCORE:
        opts_.write_cache = WriteCacheMode::Mincore;
        set_status("Flush detect: mincore");
        break;
    default:
        opts_.write_cache = WriteCacheMode::Cachestat;
        set_status("Flush detect: cachestat");
        break;
    }
    save_session();
}

void MainFrame::on_edit_verify_writes(wxCommandEvent &ev) {
    opts_.verify_writes = ev.IsChecked();
    set_status(opts_.verify_writes ? "Verify writes on" : "Verify writes off");
    save_session();
}

void MainFrame::on_log_level(wxCommandEvent &ev) {
    LogLevel min = LogLevel::Info;
    switch (ev.GetId()) {
    case ID_LOG_TRACE:
        min = LogLevel::Trace;
        break;
    case ID_LOG_DEBUG:
        min = LogLevel::Debug;
        break;
    case ID_LOG_INFO:
        min = LogLevel::Info;
        break;
    case ID_LOG_WARN:
        min = LogLevel::Warn;
        break;
    case ID_LOG_ERROR:
        min = LogLevel::Error;
        break;
    default:
        break;
    }
    if (log_)
        log_->set_min_level(min);
}

void MainFrame::on_edit_on_files(wxCommandEvent &) {
    if (opts_.mode != RunMode::Recursive) {
        int ans = alert_box(
            "This operation is risky and has not been fully tested.\n"
            "It may damage the drive or cause data loss.\n\n"
            "Switch to Files mode anyway?",
            "reflash", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (ans != wxYES) {
            if (auto *mb = GetMenuBar())
                mb->Check(ID_EDIT_RAW_DISK, true);
            return;
        }
    }
    opts_.mode = RunMode::Recursive;
    set_status(target_fs_ok_ ? wxString::Format("Files mode (%s)", target_fstype_)
                             : wxString("Files mode"));
    set_running_ui(false);
    save_session();
}

void MainFrame::on_edit_prefs(wxCommandEvent &) {
    wxDialog dlg(this, wxID_ANY, "Preferences", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);

    auto add_label = [&](const wxString &text) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
    };

    add_label("SHA-1 valid for (days)");
    auto *spin_days = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                     wxSP_ARROW_KEYS, 1, 3650, opts_.sha1_valid_days);
    grid->Add(spin_days, 0, wxEXPAND);

    add_label("Block size (0 = auto)");
    auto *spin_bs = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0, 1024 * 1024,
                                   static_cast<int>(opts_.block_size > 1024 * 1024
                                                        ? 1024 * 1024
                                                        : opts_.block_size));
    grid->Add(spin_bs, 0, wxEXPAND);

    add_label("Write cache mode");
    auto *choice_wc = new wxChoice(&dlg, wxID_ANY);
    choice_wc->Append("Write through (O_DIRECT / fdatasync)");
    choice_wc->Append("Flush via cachestat (default)");
    choice_wc->Append("Flush via mincore");
    if (opts_.write_cache == WriteCacheMode::WriteThrough)
        choice_wc->SetSelection(0);
    else if (opts_.write_cache == WriteCacheMode::Mincore)
        choice_wc->SetSelection(2);
    else
        choice_wc->SetSelection(1);
    grid->Add(choice_wc, 0, wxEXPAND);

    add_label("Flush poll interval (ms)");
    auto *spin_poll = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                     wxSP_ARROW_KEYS, 50, 60000, opts_.flush_poll_ms);
    grid->Add(spin_poll, 0, wxEXPAND);

    add_label("");
    auto *chk_verify = new wxCheckBox(&dlg, wxID_ANY, "Verify writes (O_DIRECT re-read)");
    chk_verify->SetValue(opts_.verify_writes);
    grid->Add(chk_verify, 0, wxEXPAND);

    add_label("");
    auto *chk_dry = new wxCheckBox(&dlg, wxID_ANY, "Dry-run (read-only scan)");
    chk_dry->SetValue(opts_.dry_run);
    grid->Add(chk_dry, 0, wxEXPAND);

    add_label("Run mode");
    auto *choice_mode = new wxChoice(&dlg, wxID_ANY);
    choice_mode->Append("Raw Mode (entire disk)");
    choice_mode->Append("Files Mode (supported filesystems)");
    choice_mode->SetSelection(opts_.mode == RunMode::Recursive ? 1 : 0);
    grid->Add(choice_mode, 0, wxEXPAND);

    root->Add(grid, 0, wxEXPAND | wxALL, 12);
    root->Add(new wxStaticText(
                  &dlg, wxID_ANY,
                  "SHA-1 validity defaults to 183 days (~6 months).\n"
                  "Without Write through, rewrite finishes in ~1s then full-disk sync runs\n"
                  "while the grid keeps polling; when sync ends all cells turn green."),
              0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto *btns = dlg.CreateButtonSizer(wxOK | wxCANCEL);
    root->Add(btns, 0, wxEXPAND | wxALL, 8);
    dlg.SetSizerAndFit(root);
    if (dlg.ShowModal() != wxID_OK)
        return;

    opts_.sha1_valid_days = spin_days->GetValue();
    opts_.block_size = static_cast<std::uint64_t>(spin_bs->GetValue());
    opts_.flush_poll_ms = spin_poll->GetValue();
    opts_.verify_writes = chk_verify->GetValue();
    opts_.dry_run = chk_dry->GetValue();
    switch (choice_wc->GetSelection()) {
    case 0:
        opts_.write_cache = WriteCacheMode::WriteThrough;
        break;
    case 2:
        opts_.write_cache = WriteCacheMode::Mincore;
        break;
    default:
        opts_.write_cache = WriteCacheMode::Cachestat;
        break;
    }
    RunMode new_mode = choice_mode->GetSelection() == 1 ? RunMode::Recursive : RunMode::Linear;
    if (new_mode == RunMode::Recursive && opts_.mode != RunMode::Recursive) {
        int ans = alert_box(
            "This operation is risky and has not been fully tested.\n"
            "It may damage the drive or cause data loss.\n\n"
            "Switch to Files mode anyway?",
            "reflash", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (ans != wxYES)
            new_mode = opts_.mode;
    }
    opts_.mode = new_mode;

    if (auto *mb = GetMenuBar()) {
        if (opts_.mode == RunMode::Recursive)
            mb->Check(ID_EDIT_ON_FILES, true);
        else
            mb->Check(ID_EDIT_RAW_DISK, true);
        mb->Check(ID_EDIT_DRY_RUN, opts_.dry_run);
        if (opts_.write_cache == WriteCacheMode::WriteThrough)
            mb->Check(ID_EDIT_WT_THROUGH, true);
        else if (opts_.write_cache == WriteCacheMode::Mincore)
            mb->Check(ID_EDIT_WT_MINCORE, true);
        else
            mb->Check(ID_EDIT_WT_CACHESTAT, true);
        mb->Check(ID_EDIT_VERIFY_WRITES, opts_.verify_writes);
    }

    if (db_open_) {
        std::string err;
        if (!catalog_.set_sha1_valid_days(opts_.sha1_valid_days, &err))
            alert_box(err, "reflash", wxOK | wxICON_ERROR, this);
    }
    set_status(wxString::Format("Preferences saved (SHA-1 %d days, poll %d ms)",
                                opts_.sha1_valid_days, opts_.flush_poll_ms));
    set_running_ui(false);
    save_session();
}

bool MainFrame::user_mounts_ready() const {
    /* Host fusermount ready, or we already entered the user mount namespace. */
    return probe_fuse_user_mounts().ready() || in_user_mount_ns();
}

void MainFrame::refresh_user_mounts_ui() { update_db_ui(); }

bool MainFrame::show_user_mounts_dialog() {
    wxDialog dlg(this, wxID_ANY, "User mounts support", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto *root = new wxBoxSizer(wxVERTICAL);

    /* Plain strings kept so Wrap() can be redone when the dialog grows again. */
    struct WrapText {
        wxStaticText *ctrl = nullptr;
        wxString plain;
    };
    std::vector<WrapText> wraps;

    auto add_wrap = [&](wxStaticText *ctrl, const wxString &plain) {
        wraps.push_back({ctrl, plain});
    };

    auto *hdr = new wxBoxSizer(wxHORIZONTAL);
    hdr->Add(new wxStaticBitmap(&dlg, wxID_ANY,
                                wxArtProvider::GetBitmap(wxART_TIP, wxART_MESSAGE_BOX)),
             0, wxALIGN_TOP | wxRIGHT, 10);
    auto *hdr_col = new wxBoxSizer(wxVERTICAL);
    auto *title = new wxStaticText(&dlg, wxID_ANY, "Allow browsing disk images without root");
    {
        wxFont tf = title->GetFont();
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
    }
    hdr_col->Add(title, 0, wxEXPAND);
    const wxString intro_plain =
        "reflash can open filesystem images in the Manifest & Files window. "
        "That needs a small one-time system setup. Tick what you want, then Apply.";
    auto *intro = new wxStaticText(&dlg, wxID_ANY, intro_plain);
    add_wrap(intro, intro_plain);
    hdr_col->Add(intro, 0, wxEXPAND | wxTOP, 6);
    hdr->Add(hdr_col, 1, wxEXPAND);
    root->Add(hdr, 0, wxEXPAND | wxALL, 12);

    auto add_row = [&](wxCheckBox **out, const wxArtID &art, const wxString &label,
                       const wxString &hint_plain) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticBitmap(&dlg, wxID_ANY, wxArtProvider::GetBitmap(art, wxART_MENU)), 0,
                 wxALIGN_TOP | wxRIGHT, 8);
        auto *col = new wxBoxSizer(wxVERTICAL);
        auto *chk = new wxCheckBox(&dlg, wxID_ANY, label);
        col->Add(chk, 0, wxEXPAND);
        auto *h = new wxStaticText(&dlg, wxID_ANY, hint_plain);
        {
            wxFont hf = h->GetFont();
            hf.SetPointSize(std::max(8, hf.GetPointSize() - 1));
            h->SetFont(hf);
            h->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        }
        add_wrap(h, hint_plain);
        col->Add(h, 0, wxEXPAND | wxTOP, 2);
        row->Add(col, 1, wxEXPAND);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        *out = chk;
    };

    wxCheckBox *chk_group = nullptr;
    wxCheckBox *chk_member = nullptr;
    wxCheckBox *chk_setuid = nullptr;
    wxCheckBox *chk_cap = nullptr;
    add_row(&chk_group, wxART_FOLDER, "Create the “fuse” system group",
            "A standard Linux group used for filesystem helpers. Harmless if it already exists.");
    add_row(&chk_member, wxART_GO_HOME, "Add me to the fuse group",
            "Lets your account use FUSE tools. You may need to sign out and back in afterward.");
    add_row(&chk_setuid, wxART_EXECUTABLE_FILE, "Allow fusermount to run with privileges",
            "Turns on the setuid bit so the mount helper can work for normal users.");
    add_row(&chk_cap, wxART_TICK_MARK, "Grant mount capability to fusermount",
            "Alternative to setuid: adds a mount capability. Either option is enough.");

    auto *hint = new wxStaticText(&dlg, wxID_ANY, "");
    wraps.push_back({hint, {}});
    root->Add(hint, 0, wxEXPAND | wxALL, 12);

    root->AddStretchSpacer(1);

    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    auto *btn_toggle = new wxButton(&dlg, wxID_ANY, "Turn on");
    auto *btn_apply = new wxButton(&dlg, wxID_APPLY, "Apply");
    auto *btn_close = new wxButton(&dlg, wxID_OK, "Close");
    btn_row->Add(btn_toggle, 0, wxRIGHT, 8);
    btn_row->AddStretchSpacer(1);
    btn_row->Add(btn_apply, 0, wxRIGHT, 8);
    btn_row->Add(btn_close, 0);
    root->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    dlg.SetSizer(root);
    dlg.SetMinSize(wxSize(400, 420));

    auto rewrap = [&]() {
        int w = dlg.GetClientSize().GetWidth() - 40;
        if (w < 120)
            w = 120;
        dlg.Freeze();
        for (auto &wt : wraps) {
            if (!wt.ctrl)
                continue;
            /* Reset to plain text first — Wrap() permanently inserts newlines. */
            wt.ctrl->SetLabel(wt.plain);
            if (!wt.plain.empty())
                wt.ctrl->Wrap(w);
        }
        root->Layout();
        dlg.Thaw();
    };

    auto refresh_from_probe = [&]() {
        auto st = probe_fuse_user_mounts();
        chk_group->SetValue(st.fuse_group_exists);
        chk_member->SetValue(st.user_in_fuse_group);
        chk_setuid->SetValue(st.fusermount_setuid);
        chk_cap->SetValue(st.fusermount_cap);
        if (st.ready()) {
            wraps.back().plain = "Ready — you can use Browse.";
            btn_toggle->SetLabel("Turn off");
        } else {
            wraps.back().plain =
                "Not ready yet. Tick the options you need (setuid or capability is enough), "
                "then Apply. Your system may ask for an administrator password.";
            btn_toggle->SetLabel("Turn on");
        }
        rewrap();
    };

    auto desire_from_checks = [&]() {
        FuseUserMountDesire d;
        d.fuse_group = chk_group->GetValue();
        d.member = chk_member->GetValue();
        d.setuid = chk_setuid->GetValue();
        d.cap = chk_cap->GetValue();
        return d;
    };

    dlg.Bind(wxEVT_SIZE, [&](wxSizeEvent &ev) {
        rewrap();
        ev.Skip();
    });

    /* Tall enough that the stretch spacer pins buttons to the bottom. */
    dlg.SetClientSize(520, 480);
    refresh_from_probe();
    dlg.CentreOnParent();

    auto run_priv_async = [&](std::function<bool(std::string *)> work) {
        btn_toggle->Enable(false);
        btn_apply->Enable(false);
        btn_close->Enable(false);
        auto ok = std::make_shared<bool>(false);
        auto err = std::make_shared<std::string>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread([work, ok, err, done]() {
            *ok = work(err.get());
            done->store(true);
        }).detach();
        /* Keep the GUI event loop alive so the polkit password dialog can be used. */
        while (!done->load()) {
            wxYield();
            wxMilliSleep(30);
        }
        btn_toggle->Enable(true);
        btn_apply->Enable(true);
        btn_close->Enable(true);
        refresh_from_probe();
        if (!*ok)
            alert_box(err->empty() ? "Could not change mount settings." : *err,
                      "User mounts support", wxOK | wxICON_WARNING, &dlg);
        else if (!err->empty())
            alert_box(*err, "User mounts support", wxOK | wxICON_INFORMATION, &dlg);
    };

    btn_toggle->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        run_priv_async([&](std::string *err) {
            if (probe_fuse_user_mounts().ready())
                return disable_fuse_user_mounts(err);
            FuseUserMountDesire d{true, true, true, true};
            return apply_fuse_user_mounts(d, err);
        });
    });

    btn_apply->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        auto desire = desire_from_checks();
        run_priv_async([desire](std::string *err) { return apply_fuse_user_mounts(desire, err); });
    });

    dlg.ShowModal();
    return probe_fuse_user_mounts().ready();
}

void MainFrame::on_db_admin(wxCommandEvent &) {
    if (!db_open_ || opts_.sqlite_db.empty()) {
        alert_box("Open a manifest database first.", "reflash", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const char *tools[] = {"sqlitebrowser", "sqlitestudio", nullptr};
    std::string quoted = "'";
    for (char c : opts_.sqlite_db) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    for (int i = 0; tools[i]; ++i) {
        std::string cmd = std::string(tools[i]) + " " + quoted;
        long pid = wxExecute(wxString::FromUTF8(cmd), wxEXEC_ASYNC);
        if (pid != 0) {
            set_status(wxString::Format("SQLite admin: %s", tools[i]));
            return;
        }
    }
    alert_box(
        "No SQLite GUI found.\nInstall sqlitebrowser (recommended) or sqlitestudio,\n"
        "then try Manifest → SQLite admin again.",
        "SQLite admin", wxOK | wxICON_WARNING, this);
}

void MainFrame::on_db_user_mounts(wxCommandEvent &) {
    show_user_mounts_dialog();
    update_db_ui();
}

void MainFrame::on_help_about(wxCommandEvent &) {
    wxAboutDialogInfo info;
    info.SetName("reflash");
    info.SetVersion(PROJECT_VERSION);
    info.SetDescription("Rewrite device/file data in place to refresh flash storage.");
    info.SetCopyright(wxString::Format("(C) %d %s", PROJECT_YEAR, PROJECT_AUTHOR));
    info.SetWebSite("https://github.com/lenik/reflash");
    info.AddDeveloper(PROJECT_AUTHOR);
    wxAboutBox(info, this);
}

void MainFrame::on_help_usage(wxCommandEvent &) {
    alert_box(
        "Choose a Drive (Disks / Loop / Image File), Use a manifest database,\n"
        "then Start. Dry-run reads without writing.\n"
        "SHA-1 runs only when a manifest database is in use.\n\n"
        "Headless: reflash [OPTIONS] DEVICE/FILE\n"
        "GUI idle: reflash -g\n\n"
        "The GUI does not ask questions on the terminal.",
        "Usage", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::on_start(wxCommandEvent &) { start_run(); }

void MainFrame::on_pause(wxCommandEvent &) {
    if (!engine_ || !engine_->running())
        return;
    if (engine_->progress().input_locked())
        return;
    if (!paused_) {
        engine_->pause().pause();
        engine_->progress().set_paused(true);
        paused_ = true;
        set_pause_label(true);
        set_work_state(WORK_PAUSE);
        set_status("Paused");
    } else {
        engine_->pause().resume();
        engine_->progress().set_paused(false);
        paused_ = false;
        set_pause_label(false);
        set_work_state(WORK_RUN);
        set_status("Running");
    }
}

void MainFrame::on_stop(wxCommandEvent &) {
    if (!engine_ || !engine_->running())
        return;
    if (engine_->progress().input_locked())
        return;
    engine_->pause().request_stop();
    engine_->pause().resume();
    set_status("Stopping...");
}

void MainFrame::on_timer(wxTimerEvent &) {
    if (!engine_)
        return;
    auto snap = engine_->progress().snapshot();
    if (snap.bytes_total > 0) {
        int v = static_cast<int>((snap.bytes_done * 1000) / snap.bytes_total);
        gauge_->SetValue(std::min(1000, v));
    }
    double pct =
        snap.bytes_total ? (100.0 * static_cast<double>(snap.bytes_done) / snap.bytes_total) : 0.0;
    set_field(SF_PHASE, snap.paused ? snap.phase + " paused" : snap.phase);
    set_field(SF_PCT, wxString::Format("%.1f%%", pct));
    set_field(SF_RATE, format_rate(snap.bytes_per_sec));
    set_field(SF_ETA, format_eta(snap.eta_seconds));
    set_field(SF_CELLS, wxString::Format("%zu / shift %u", snap.cells.size(), snap.cell_shift));
    grid_->update_cells(snap.cells);

    for (const auto &e : engine_->progress().take_logs()) {
        log_->append(e);
        mirror_log_stdout(e);
    }

    if (snap.input_locked) {
        SetCursor(wxCURSOR_WAIT);
        if (auto *mb = GetMenuBar()) {
            enable_menu(mb, ID_PAUSE, false);
            enable_menu(mb, ID_STOP, false);
        }
        if (toolbar_) {
            if (toolbar_->FindById(ID_PAUSE))
                toolbar_->EnableTool(ID_PAUSE, false);
            if (toolbar_->FindById(ID_STOP))
                toolbar_->EnableTool(ID_STOP, false);
        }
    } else if (engine_->running()) {
        SetCursor(wxNullCursor);
        if (auto *mb = GetMenuBar()) {
            enable_menu(mb, ID_PAUSE, true);
            enable_menu(mb, ID_STOP, true);
        }
        if (toolbar_) {
            if (toolbar_->FindById(ID_PAUSE))
                toolbar_->EnableTool(ID_PAUSE, true);
            if (toolbar_->FindById(ID_STOP))
                toolbar_->EnableTool(ID_STOP, true);
        }
    }

    if (!snap.finished && !snap.phase.empty())
        set_status(snap.paused ? "Paused" : wxString::FromUTF8(snap.phase));

    if (snap.finished || !engine_->running()) {
        timer_.Stop();
        engine_->join();
        snap = engine_->progress().snapshot();
        grid_->update_cells(snap.cells);
        for (const auto &e : engine_->progress().take_logs()) {
            log_->append(e);
            mirror_log_stdout(e);
        }
        paused_ = false;
        SetCursor(wxNullCursor);
        set_running_ui(false);
        if (snap.failed || !snap.finished) {
            set_work_state(WORK_ERR);
            set_status(snap.failed ? "Failed" : "Stopped");
        } else {
            set_work_state(WORK_OK);
            set_status("Finished");
        }
        if (snap.finished)
            maybe_offer_badblocks();
    }
}

void MainFrame::maybe_offer_badblocks() {
    if (offered_bad_ || !engine_)
        return;
    offered_bad_ = true;
    auto bad = engine_->store().bad_extents();
    if (bad.empty())
        return;
    int ans = alert_box(
        wxString::Format(
            "Recorded %zu bad extent(s).\nExport a badblocks-compatible list of block numbers?",
            bad.size()),
        "reflash", wxYES_NO | wxICON_WARNING, this);
    if (ans != wxYES)
        return;
    wxFileDialog dlg(this, "Save bad block list", "", "badblocks.txt", "Text files (*.txt)|*.txt",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::ofstream out(dlg.GetPath().ToStdString());
    std::uint64_t bs = engine_->progress().block_size();
    if (bs == 0)
        bs = 512;
    for (const auto &e : bad) {
        std::uint64_t start = static_cast<std::uint64_t>(e.offset) / bs;
        std::uint64_t end =
            (static_cast<std::uint64_t>(e.offset + e.length) + bs - 1) / bs;
        for (std::uint64_t b = start; b < end; ++b)
            out << b << '\n';
    }
    alert_box("Wrote bad block list. For ext filesystems you may pass it to e2fsck -l.\n"
                 "Do not mark blocks without understanding the filesystem consequences.",
                 "reflash", wxOK | wxICON_INFORMATION, this);
}

} /* namespace reflash */
