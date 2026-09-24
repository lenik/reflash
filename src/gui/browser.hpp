/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "db/store.hpp"
#include "gui/session.hpp"
#include "mount/automount.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wx/frame.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/statbmp.h>
#include <wx/toolbar.h>
#include <wx/treectrl.h>

namespace reflash {

class BrowserFrame : public wxFrame {
public:
    /* fs_root is a directory. Optionally owns a temporary mount of device_path. */
    BrowserFrame(wxWindow *parent, Store *store, const std::string &fs_root,
                 const std::string &device_path = {}, std::vector<MountRecord> mounts = {},
                 bool temp_mount = false);
    ~BrowserFrame() override;

private:
    enum SbField { SF_PATH = 0, SF_SCAN, SF_RECORDS, SF_ICON, SF_COUNT };
    enum WorkState { WORK_IDLE = 0, WORK_RUN, WORK_SYNC, WORK_OK, WORK_ERR };
    enum ViewMode { VIEW_ICON = 0, VIEW_LIST, VIEW_COMPACT };
    enum ArrangeBy { ARR_MANUAL = 0, ARR_NAME, ARR_SIZE, ARR_TYPE, ARR_MTIME };
    static constexpr int kZoomMin = -2;
    static constexpr int kZoomMax = 3;

    struct Node : wxTreeItemData {
        std::string rel; /* "" = root, else "a/b" */
        std::int64_t folder_id = 0;
    };

    struct ListRow {
        std::string name;
        bool is_dir = false;
        bool in_fs = false;
        bool in_db = false;
        DirDbEntry db;
        std::string abs;
        std::string db_path;
        std::int64_t mtime = 0;
        int manual_order = 0;
        int icon_index = 1;
        wxString status; /* report-mode status column */
    };

    struct VerifyJob {
        std::uint64_t gen = 0;
        long row = -1;
        std::string abs;
        std::string db_path;
    };

    struct QueueMsg {
        enum Kind { Cancel, Verify } kind = Cancel;
        VerifyJob job;
    };

    Store *store_ = nullptr;
    std::string fs_root_;
    std::string device_path_;
    std::vector<MountRecord> mounts_;
    bool temp_mount_ = false;
    wxTreeCtrl *tree_ = nullptr;
    wxPanel *list_host_ = nullptr;
    wxListCtrl *list_ = nullptr;
    wxToolBar *view_tb_ = nullptr;
    wxImageList *tree_icons_ = nullptr;
    wxImageList *list_icons_ = nullptr;
    wxTreeItemId root_item_;
    std::vector<ListRow> list_rows_;
    int icon_folder_ = 0;
    int icon_file_ = 1;
    bool tree_sel_guard_ = false;
    std::string pending_tree_rel_;
    bool pending_tree_show_ = false;

    ViewMode view_mode_ = VIEW_LIST;
    ArrangeBy arrange_ = ARR_NAME;
    bool arrange_rev_ = false;
    bool show_hidden_ = false;
    bool show_thumbs_ = false;
    int zoom_level_ = 0;
    int auto_rewrite_months_ = 6;

    wxStaticBitmap *field_icons_[SF_ICON] = {};
    wxStaticBitmap *work_icon_ = nullptr;
    wxString path_shown_;
    std::atomic<std::uint64_t> scanned_files_{0};
    std::atomic<std::uint64_t> scanned_bytes_{0};
    std::atomic<int> pending_hashes_{0};
    std::atomic<int> work_state_{WORK_IDLE};

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<QueueMsg> queue_;
    bool stop_worker_ = false;
    std::thread worker_;
    std::atomic<std::uint64_t> epoch_{1};

    void build_ui();
    void build_tree_icons();
    void rebuild_list_icons();
    void build_statusbar();
    void apply_view_mode();
    void apply_list_font();
    void update_view_ui();
    void sort_list_rows();
    void fill_list_ctrl(bool enqueue_verify);
    void refresh_current_folder();
    int list_icon_size() const;
    bool is_image_name(const std::string &name) const;
    int add_row_icon(const ListRow &row);
    void populate_tree_children(const wxTreeItemId &item);
    void show_folder(const Node &node);
    void schedule_show_folder(const Node &node);
    wxTreeItemId find_tree_by_rel(const std::string &rel);
    void restore_list_selection(const std::vector<std::string> &names);
    void enqueue_verifies(std::uint64_t gen, std::vector<VerifyJob> jobs);
    void worker_main();
    void on_tree_expanding(wxTreeEvent &ev);
    void on_tree_sel(wxTreeEvent &ev);
    void on_tree_menu(wxTreeEvent &ev);
    void on_list_activate(wxListEvent &ev);
    void on_list_menu(wxListEvent &ev);
    void on_list_wheel(wxMouseEvent &ev);
    void on_refresh(wxCommandEvent &);
    void on_up(wxCommandEvent &);
    void on_close(wxCommandEvent &);
    void on_sync_folder(wxCommandEvent &);
    void on_sync_selection(wxCommandEvent &);
    void on_open(wxCommandEvent &);
    void on_open_as(wxCommandEvent &);
    void on_view_reset(wxCommandEvent &);
    void on_view_hidden(wxCommandEvent &);
    void on_view_thumbs(wxCommandEvent &);
    void on_view_arrange(wxCommandEvent &);
    void on_view_reverse(wxCommandEvent &);
    void on_view_zoom_in(wxCommandEvent &);
    void on_view_zoom_out(wxCommandEvent &);
    void on_view_zoom_normal(wxCommandEvent &);
    void on_view_mode(wxCommandEvent &);
    void on_prefs(wxCommandEvent &);
    void load_settings();
    void save_settings();
    BrowserSettings current_settings() const;
    void apply_auto_rewrite_months(int months);
    void set_zoom_level(int level);
    void set_view_mode(ViewMode mode);
    void set_path_status(const wxString &path);
    void note_scanned(std::uint64_t bytes);
    void set_work_state(int state);
    void refresh_statusbar();
    void place_field_icons();
    void place_work_icon();
    wxStaticBitmap *make_field_icon(wxStatusBar *bar, const char *art);
    void sync_rows(const std::vector<ListRow> &rows);
    void open_path(const std::string &path, bool pick_app);
    std::string selected_file_path() const;
    Node *selected_node();
};

} /* namespace reflash */
