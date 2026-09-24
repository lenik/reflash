/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/alert.hpp"
#include "gui/browser.hpp"
#include "gui/session.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wx/accel.h>
#include <wx/artprov.h>
#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/toolbar.h>
#include <wx/utils.h>

namespace sdmsg {

namespace {

enum {
    ID_BR_REFRESH = wxID_HIGHEST + 800,
    ID_BR_UP,
    ID_BR_SYNC,
    ID_BR_SYNC_SEL,
    ID_BR_OPEN,
    ID_BR_OPEN_AS,
    ID_VIEW_RESET,
    ID_VIEW_HIDDEN,
    ID_VIEW_THUMBS,
    ID_ARR_MANUAL,
    ID_ARR_NAME,
    ID_ARR_SIZE,
    ID_ARR_TYPE,
    ID_ARR_MTIME,
    ID_ARR_REVERSE,
    ID_VIEW_ZOOM_IN,
    ID_VIEW_ZOOM_OUT,
    ID_VIEW_ZOOM_NORMAL,
    ID_VIEW_ICON,
    ID_VIEW_LIST,
    ID_VIEW_COMPACT,
    ID_BR_PREFS,
};

wxString mark_ok() { return wxString::FromUTF8("\xE2\x9C\x85"); }
wxString mark_bad() { return wxString::FromUTF8("\xE2\x9D\x8C"); }

wxString sha1_hex(const unsigned char *dig, bool has) {
    if (!has || !dig)
        return {};
    wxString s;
    s.reserve(40);
    for (int i = 0; i < 20; ++i)
        s += wxString::Format("%02x", dig[i]);
    return s;
}

bool hash_file(const std::string &path, unsigned char out[20], std::uint64_t *bytes_out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
        if (ctx)
            EVP_MD_CTX_free(ctx);
        close(fd);
        return false;
    }
    unsigned char buf[1 << 16];
    std::uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            EVP_MD_CTX_free(ctx);
            close(fd);
            return false;
        }
        if (n == 0)
            break;
        total += static_cast<std::uint64_t>(n);
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n));
    }
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
    close(fd);
    if (bytes_out)
        *bytes_out = total;
    return len == 20;
}

wxString format_bytes(std::uint64_t n) {
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double mb = 1024.0 * 1024.0;
    const double kb = 1024.0;
    if (n >= static_cast<std::uint64_t>(gb))
        return wxString::Format("%.1f GB", static_cast<double>(n) / gb);
    if (n >= static_cast<std::uint64_t>(mb))
        return wxString::Format("%.1f MB", static_cast<double>(n) / mb);
    if (n >= static_cast<std::uint64_t>(kb))
        return wxString::Format("%.1f KB", static_cast<double>(n) / kb);
    return wxString::Format("%llu B", static_cast<unsigned long long>(n));
}

wxString format_count(std::int64_t n) {
    wxString s = wxString::Format("%lld", static_cast<long long>(n));
    int insert = static_cast<int>(s.length()) - 3;
    while (insert > 0) {
        s.insert(static_cast<size_t>(insert), ',');
        insert -= 3;
    }
    return s;
}

wxBitmap work_bitmap(int state) {
    const char *art = wxART_INFORMATION;
    switch (state) {
    case 1: /* WORK_RUN */
        art = wxART_GO_FORWARD;
        break;
    case 2: /* WORK_SYNC */
        art = wxART_EXECUTABLE_FILE;
        break;
    case 3: /* WORK_OK */
        art = wxART_TICK_MARK;
        break;
    case 4: /* WORK_ERR */
        art = wxART_ERROR;
        break;
    default:
        art = wxART_INFORMATION;
        break;
    }
    return wxArtProvider::GetBitmap(art, wxART_MENU, wxSize(16, 16));
}

/* Icon-only view switchers: 3x3 grid, list rows, 2x3 compact. */
wxBitmap make_view_icon_bitmap(int kind /*0 icon, 1 list, 2 compact*/, int sz = 16) {
    wxBitmap bmp(sz, sz);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
    dc.Clear();
    dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT), 1));
    dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    if (kind == 0) {
        /* 3×3 dots */
        int gap = sz / 4;
        int r = std::max(1, sz / 10);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                int x = gap + col * gap;
                int y = gap + row * gap;
                dc.DrawCircle(x, y, r);
            }
        }
    } else if (kind == 1) {
        /* bullet + line ×3 */
        int y0 = sz / 5;
        int step = sz / 4;
        int r = std::max(1, sz / 12);
        for (int i = 0; i < 3; ++i) {
            int y = y0 + i * step;
            dc.DrawCircle(sz / 5, y, r);
            dc.DrawLine(sz / 3, y, sz - 2, y);
        }
    } else {
        /* 2×3 dots (compact) */
        int gapx = sz / 3;
        int gapy = sz / 4;
        int r = std::max(1, sz / 10);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 2; ++col) {
                int x = gapx + col * gapx;
                int y = gapy + row * gapy;
                dc.DrawCircle(x, y, r);
            }
        }
    }
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

std::string join_root(const std::string &root, const std::string &rel) {
    if (rel.empty())
        return root;
    if (!root.empty() && root.back() == '/')
        return root + rel;
    return root + "/" + rel;
}

std::string db_path_for(const std::string &rel, const std::string &name) {
    if (rel.empty())
        return "/" + name;
    return "/" + rel + "/" + name;
}

enum ConflictChoice { ConflictDelete = 0, ConflictOverwrite = 1, ConflictIgnore = 2 };

struct ConflictResult {
    int choice = ConflictIgnore;
    bool apply_rest = false;
    bool remember = false;
};

ConflictResult ask_conflict(wxWindow *parent, const std::string &path) {
    ConflictResult out;
    int saved = -1;
    bool remember = false;
    load_conflict_pref(&saved, &remember);
    if (remember && saved >= ConflictDelete && saved <= ConflictIgnore) {
        out.choice = saved;
        out.apply_rest = true;
        out.remember = true;
        return out;
    }

    wxDialog dlg(parent, wxID_ANY, "SHA-1 conflict", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);
    auto *root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&dlg, wxID_ANY,
                               wxString::Format("File differs from the database record:\n%s\n\n"
                                                "Choose how to resolve this conflict.",
                                                path)),
              0, wxALL, 12);
    auto *btns = new wxBoxSizer(wxVERTICAL);
    auto *del = new wxButton(&dlg, wxID_HIGHEST + 1, "Delete corrupted file");
    auto *over = new wxButton(&dlg, wxID_HIGHEST + 2, "Use current file SHA-1 (overwrite DB)");
    auto *ign = new wxButton(&dlg, wxID_HIGHEST + 3, "Ignore (keep conflict)");
    btns->Add(del, 0, wxEXPAND | wxBOTTOM, 4);
    btns->Add(over, 0, wxEXPAND | wxBOTTOM, 4);
    btns->Add(ign, 0, wxEXPAND | wxBOTTOM, 8);
    auto *apply = new wxCheckBox(&dlg, wxID_ANY, "Use the same choice for the rest of this Sync");
    auto *dont = new wxCheckBox(&dlg, wxID_ANY, "Remember and do not ask again");
    btns->Add(apply, 0, wxEXPAND | wxBOTTOM, 4);
    btns->Add(dont, 0, wxEXPAND);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    dlg.SetSizerAndFit(root);

    auto finish = [&](int choice) {
        out.choice = choice;
        out.apply_rest = apply->GetValue();
        out.remember = dont->GetValue();
        if (out.remember)
            save_conflict_pref(choice, true);
        dlg.EndModal(wxID_OK);
    };
    del->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { finish(ConflictDelete); });
    over->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { finish(ConflictOverwrite); });
    ign->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { finish(ConflictIgnore); });
    dlg.ShowModal();
    return out;
}

} /* namespace */

BrowserFrame::BrowserFrame(wxWindow *parent, Store *store, const std::string &fs_root,
                           const std::string &device_path, std::vector<MountRecord> mounts,
                           bool temp_mount)
    : wxFrame(parent, wxID_ANY, "Manifest & Files", wxDefaultPosition, wxSize(980, 640)),
      store_(store), fs_root_(fs_root), device_path_(device_path), mounts_(std::move(mounts)),
      temp_mount_(temp_mount) {
    load_settings();
    build_ui();
    apply_auto_rewrite_months(auto_rewrite_months_);
    worker_ = std::thread([this] { worker_main(); });
    populate_tree_children(root_item_);
    tree_->Expand(root_item_);
    if (auto *data = dynamic_cast<Node *>(tree_->GetItemData(root_item_)))
        show_folder(*data);
}

BrowserFrame::~BrowserFrame() {
    save_settings();
    {
        std::lock_guard<std::mutex> lock(qmu_);
        stop_worker_ = true;
        queue_.clear();
    }
    qcv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (temp_mount_ && !device_path_.empty()) {
        std::string err;
        restore_mount_state(device_path_, {}, mounts_, true, &err);
    }
    if (tree_)
        tree_->SetImageList(nullptr);
    if (list_) {
        list_->SetImageList(nullptr, wxIMAGE_LIST_SMALL);
        list_->SetImageList(nullptr, wxIMAGE_LIST_NORMAL);
    }
    delete tree_icons_;
    tree_icons_ = nullptr;
    delete list_icons_;
    list_icons_ = nullptr;
}

void BrowserFrame::build_tree_icons() {
    tree_icons_ = new wxImageList(16, 16, true);
    tree_icons_->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_MENU, wxSize(16, 16)));
    tree_icons_->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_MENU, wxSize(16, 16)));
    icon_folder_ = 0;
    icon_file_ = 1;
}

int BrowserFrame::list_icon_size() const {
    static const int icon_sizes[] = {32, 48, 64, 96, 128, 160};
    static const int small_sizes[] = {12, 14, 16, 18, 22, 28};
    int idx = zoom_level_ - kZoomMin;
    if (idx < 0)
        idx = 0;
    if (idx > 5)
        idx = 5;
    return view_mode_ == VIEW_ICON ? icon_sizes[idx] : small_sizes[idx];
}

bool BrowserFrame::is_image_name(const std::string &name) const {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
        return false;
    std::string ext = name.substr(dot + 1);
    for (char &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp" ||
           ext == "webp" || ext == "tif" || ext == "tiff" || ext == "ico" || ext == "xpm";
}

void BrowserFrame::rebuild_list_icons() {
    if (list_) {
        list_->SetImageList(nullptr, wxIMAGE_LIST_SMALL);
        list_->SetImageList(nullptr, wxIMAGE_LIST_NORMAL);
    }
    delete list_icons_;
    const int sz = list_icon_size();
    list_icons_ = new wxImageList(sz, sz, true);
    list_icons_->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(sz, sz)));
    list_icons_->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_OTHER, wxSize(sz, sz)));
    icon_folder_ = 0;
    icon_file_ = 1;
    if (!list_)
        return;
    if (view_mode_ == VIEW_ICON)
        list_->SetImageList(list_icons_, wxIMAGE_LIST_NORMAL);
    else
        list_->SetImageList(list_icons_, wxIMAGE_LIST_SMALL);
}

int BrowserFrame::add_row_icon(const ListRow &row) {
    if (!list_icons_)
        return row.is_dir ? icon_folder_ : icon_file_;
    if (show_thumbs_ && row.in_fs && !row.is_dir && is_image_name(row.name)) {
        wxLogNull quiet;
        wxImage img;
        if (!img.LoadFile(row.abs, wxBITMAP_TYPE_ANY) && is_image_name(row.name)) {
            /* Some .xpm files need an explicit type. */
            img.LoadFile(row.abs, wxBITMAP_TYPE_XPM);
        }
        if (img.IsOk()) {
            const int sz = list_icon_size();
            img.Rescale(sz, sz, wxIMAGE_QUALITY_NEAREST);
            return list_icons_->Add(wxBitmap(img));
        }
    }
    return row.is_dir ? icon_folder_ : icon_file_;
}

void BrowserFrame::build_ui() {
    build_tree_icons();
    rebuild_list_icons();

    auto *menubar = new wxMenuBar();
    auto *file = new wxMenu();
    file->Append(ID_BR_UP, "&Up\tAlt+Up");
    file->Append(ID_BR_REFRESH, "&Refresh\tF5");
    file->AppendSeparator();
    file->Append(ID_BR_SYNC, "&Sync\tCtrl+S");
    file->Append(ID_BR_OPEN, "&Open\tCtrl+Return");
    file->Append(ID_BR_OPEN_AS, "Open &as...\tCtrl+Shift+O");
    file->AppendSeparator();
    file->Append(wxID_CLOSE, "&Close\tCtrl+W");
    menubar->Append(file, "&File");

    auto *edit = new wxMenu();
    edit->Append(ID_BR_PREFS, "&Preferences...\tCtrl+,");
    menubar->Append(edit, "&Edit");

    auto *view = new wxMenu();
    view->Append(ID_VIEW_RESET, "Reset views to &Default");
    view->AppendCheckItem(ID_VIEW_HIDDEN, "Show &hidden files\tCtrl+H");
    view->AppendCheckItem(ID_VIEW_THUMBS, "Show &thumbnails");
    view->AppendSeparator();
    auto *arrange = new wxMenu();
    arrange->AppendRadioItem(ID_ARR_MANUAL, "&Manually");
    arrange->AppendRadioItem(ID_ARR_NAME, "&Name");
    arrange->AppendRadioItem(ID_ARR_SIZE, "&Size");
    arrange->AppendRadioItem(ID_ARR_TYPE, "&Type");
    arrange->AppendRadioItem(ID_ARR_MTIME, "&Modification date");
    arrange->AppendSeparator();
    arrange->AppendCheckItem(ID_ARR_REVERSE, "&Reversed order");
    view->AppendSubMenu(arrange, "&Arrange Items");
    view->AppendSeparator();
    view->Append(ID_VIEW_ZOOM_IN, "Zoom &In\tCtrl++");
    view->Append(ID_VIEW_ZOOM_OUT, "Zoom &Out\tCtrl+-");
    view->Append(ID_VIEW_ZOOM_NORMAL, "&Normal size\tCtrl+0");
    view->AppendSeparator();
    view->AppendRadioItem(ID_VIEW_ICON, "&Icon\tCtrl+1");
    view->AppendRadioItem(ID_VIEW_LIST, "&List\tCtrl+2");
    view->AppendRadioItem(ID_VIEW_COMPACT, "&Compact\tCtrl+3");
    menubar->Append(view, "&View");

    auto *help = new wxMenu();
    help->Append(wxID_ABOUT, "&About\tF1");
    menubar->Append(help, "&Help");
    SetMenuBar(menubar);

    auto *tb = CreateToolBar(wxTB_HORIZONTAL | wxTB_TEXT);
    tb->AddTool(ID_BR_UP, "Up", wxArtProvider::GetBitmap(wxART_GO_UP, wxART_TOOLBAR));
    tb->AddTool(ID_BR_REFRESH, "Refresh", wxArtProvider::GetBitmap(wxART_REDO, wxART_TOOLBAR));
    tb->AddTool(ID_BR_SYNC, "Sync", wxArtProvider::GetBitmap(wxART_EXECUTABLE_FILE, wxART_TOOLBAR));
    tb->AddSeparator();
    tb->AddTool(wxID_CLOSE, "Close", wxArtProvider::GetBitmap(wxART_QUIT, wxART_TOOLBAR));
    tb->Realize();

    CreateStatusBar(SF_COUNT);
    build_statusbar();

    auto *split = new wxSplitterWindow(this, wxID_ANY);
    tree_ = new wxTreeCtrl(split, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxTR_DEFAULT_STYLE | wxTR_HAS_BUTTONS);
    tree_->SetImageList(tree_icons_);

    list_host_ = new wxPanel(split);
    auto *list_sizer = new wxBoxSizer(wxVERTICAL);
    auto *top = new wxBoxSizer(wxHORIZONTAL);
    top->AddStretchSpacer(1);
    view_tb_ = new wxToolBar(list_host_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxTB_HORIZONTAL | wxTB_FLAT | wxTB_NODIVIDER);
    view_tb_->AddRadioTool(ID_VIEW_ICON, wxEmptyString, make_view_icon_bitmap(0), wxNullBitmap,
                           "Icons");
    view_tb_->AddRadioTool(ID_VIEW_LIST, wxEmptyString, make_view_icon_bitmap(1), wxNullBitmap,
                           "List");
    view_tb_->AddRadioTool(ID_VIEW_COMPACT, wxEmptyString, make_view_icon_bitmap(2), wxNullBitmap,
                           "Compact");
    view_tb_->Realize();
    top->Add(view_tb_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    list_sizer->Add(top, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);

    list_ = new wxListCtrl(list_host_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    list_sizer->Add(list_, 1, wxEXPAND);
    list_host_->SetSizer(list_sizer);
    apply_view_mode();

    split->SplitVertically(tree_, list_host_, 280);

    auto *root = new Node();
    root->rel = "";
    root->folder_id = 0;
    wxString label = device_path_.empty() ? wxString(fs_root_) : wxString(device_path_);
    root_item_ = tree_->AddRoot(label, 0, 0, root);

    Bind(wxEVT_TREE_ITEM_EXPANDING, &BrowserFrame::on_tree_expanding, this);
    Bind(wxEVT_TREE_SEL_CHANGED, &BrowserFrame::on_tree_sel, this);
    Bind(wxEVT_TREE_ITEM_MENU, &BrowserFrame::on_tree_menu, this);
    Bind(wxEVT_LIST_ITEM_ACTIVATED, &BrowserFrame::on_list_activate, this);
    Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &BrowserFrame::on_list_menu, this);
    list_->Bind(wxEVT_MOUSEWHEEL, &BrowserFrame::on_list_wheel, this);

    Bind(wxEVT_MENU, &BrowserFrame::on_refresh, this, ID_BR_REFRESH);
    Bind(wxEVT_MENU, &BrowserFrame::on_up, this, ID_BR_UP);
    Bind(wxEVT_MENU, &BrowserFrame::on_close, this, wxID_CLOSE);
    Bind(wxEVT_MENU, &BrowserFrame::on_sync_folder, this, ID_BR_SYNC);
    Bind(wxEVT_MENU, &BrowserFrame::on_sync_selection, this, ID_BR_SYNC_SEL);
    Bind(wxEVT_MENU, &BrowserFrame::on_open, this, ID_BR_OPEN);
    Bind(wxEVT_MENU, &BrowserFrame::on_open_as, this, ID_BR_OPEN_AS);
    Bind(wxEVT_TOOL, &BrowserFrame::on_refresh, this, ID_BR_REFRESH);
    Bind(wxEVT_TOOL, &BrowserFrame::on_up, this, ID_BR_UP);
    Bind(wxEVT_TOOL, &BrowserFrame::on_sync_folder, this, ID_BR_SYNC);
    Bind(wxEVT_TOOL, &BrowserFrame::on_close, this, wxID_CLOSE);

    Bind(wxEVT_MENU, &BrowserFrame::on_view_reset, this, ID_VIEW_RESET);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_hidden, this, ID_VIEW_HIDDEN);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_thumbs, this, ID_VIEW_THUMBS);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_arrange, this, ID_ARR_MANUAL, ID_ARR_MTIME);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_reverse, this, ID_ARR_REVERSE);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_zoom_in, this, ID_VIEW_ZOOM_IN);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_zoom_out, this, ID_VIEW_ZOOM_OUT);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_zoom_normal, this, ID_VIEW_ZOOM_NORMAL);
    Bind(wxEVT_MENU, &BrowserFrame::on_view_mode, this, ID_VIEW_ICON, ID_VIEW_COMPACT);
    Bind(wxEVT_TOOL, &BrowserFrame::on_view_mode, this, ID_VIEW_ICON, ID_VIEW_COMPACT);
    Bind(wxEVT_MENU, &BrowserFrame::on_prefs, this, ID_BR_PREFS);

    Bind(wxEVT_MENU, [this](wxCommandEvent &) {
        alert_box("Files on disk and in the database.\n"
                  "New: on disk only. Grey: in the database only.\n"
                  "Sync: hash new files, drop dangling DB rows, and ask about conflicts.\n"
                  "Open uses the desktop default; Open as picks a program.\n"
                  "Ctrl+Mouse wheel zooms the file list.",
                  "sdmsg files", wxOK | wxICON_INFORMATION, this);
    }, wxID_ABOUT);

    /* Also accept Ctrl+= as zoom-in (unshifted + on many keyboards). */
    wxAcceleratorEntry accels[] = {
        wxAcceleratorEntry(wxACCEL_CTRL, '=', ID_VIEW_ZOOM_IN),
        wxAcceleratorEntry(wxACCEL_CTRL, WXK_NUMPAD_ADD, ID_VIEW_ZOOM_IN),
        wxAcceleratorEntry(wxACCEL_CTRL, WXK_NUMPAD_SUBTRACT, ID_VIEW_ZOOM_OUT),
    };
    SetAcceleratorTable(wxAcceleratorTable(3, accels));

    update_view_ui();
    set_path_status(fs_root_);
    refresh_statusbar();
}

void BrowserFrame::build_statusbar() {
    auto *bar = GetStatusBar();
    if (!bar)
        return;
    static const int widths[SF_COUNT] = {-1, 220, 160, 28};
    bar->SetFieldsCount(SF_COUNT);
    bar->SetStatusWidths(SF_COUNT, widths);

    field_icons_[SF_PATH] = make_field_icon(bar, wxART_FOLDER);
    field_icons_[SF_SCAN] = make_field_icon(bar, wxART_EXECUTABLE_FILE);
    field_icons_[SF_RECORDS] = make_field_icon(bar, wxART_NORMAL_FILE);
    work_icon_ = new wxStaticBitmap(bar, wxID_ANY, work_bitmap(WORK_IDLE));

    bar->Bind(wxEVT_SIZE, [this](wxSizeEvent &ev) {
        place_field_icons();
        place_work_icon();
        ev.Skip();
    });
    CallAfter([this] {
        place_field_icons();
        place_work_icon();
    });
}

wxStaticBitmap *BrowserFrame::make_field_icon(wxStatusBar *bar, const char *art) {
    wxBitmap bmp = wxArtProvider::GetBitmap(art, wxART_MENU, wxSize(14, 14));
    auto *ico = new wxStaticBitmap(bar, wxID_ANY, bmp);
    ico->Hide();
    return ico;
}

void BrowserFrame::place_field_icons() {
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
        ico->SetSize(r.x + 3, r.y + (r.height - sz.y) / 2, sz.x, sz.y);
        ico->Show();
    }
}

void BrowserFrame::place_work_icon() {
    auto *bar = GetStatusBar();
    if (!bar || !work_icon_)
        return;
    wxRect r;
    if (!bar->GetFieldRect(SF_ICON, r))
        return;
    wxSize sz = work_icon_->GetSize();
    work_icon_->Move(r.x + (r.width - sz.x) / 2, r.y + (r.height - sz.y) / 2);
}

void BrowserFrame::set_work_state(int state) {
    work_state_.store(state);
    if (work_icon_)
        work_icon_->SetBitmap(work_bitmap(state));
    place_work_icon();
}

void BrowserFrame::set_path_status(const wxString &path) {
    path_shown_ = path;
    refresh_statusbar();
}

void BrowserFrame::note_scanned(std::uint64_t bytes) {
    scanned_files_.fetch_add(1);
    scanned_bytes_.fetch_add(bytes);
}

void BrowserFrame::refresh_statusbar() {
    auto *bar = GetStatusBar();
    if (!bar)
        return;

    wxString path = path_shown_;
    if (!path.empty())
        path = "    " + path;
    bar->SetStatusText(path, SF_PATH);

    std::uint64_t nfiles = scanned_files_.load();
    std::uint64_t nbytes = scanned_bytes_.load();
    wxString scan = wxString::Format("%llu files / %s scanned",
                                     static_cast<unsigned long long>(nfiles), format_bytes(nbytes));
    bar->SetStatusText("    " + scan, SF_SCAN);

    std::int64_t records = store_ ? store_->file_record_count() : 0;
    wxString rec = format_count(records) + " records";
    bar->SetStatusText("    " + rec, SF_RECORDS);

    place_field_icons();
    place_work_icon();
}

BrowserFrame::Node *BrowserFrame::selected_node() {
    wxTreeItemId cur = tree_->GetSelection();
    if (!cur.IsOk())
        cur = root_item_;
    return dynamic_cast<Node *>(tree_->GetItemData(cur));
}

void BrowserFrame::populate_tree_children(const wxTreeItemId &item) {
    if (!item.IsOk())
        return;
    if (tree_->GetChildrenCount(item, false) > 0)
        return;
    auto *node = dynamic_cast<Node *>(tree_->GetItemData(item));
    if (!node)
        return;
    std::string dir = join_root(fs_root_, node->rel);
    const bool use_db = store_ && (node->folder_id != 0 || node->rel.empty());
    std::map<std::string, std::int64_t> db_dirs;
    if (use_db) {
        for (const auto &e : store_->list_db_dir(node->folder_id)) {
            if (e.directory)
                db_dirs[e.basename] = e.id;
        }
    }
    std::map<std::string, bool> names;
    bool denied = false;
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (errno == EACCES || errno == EPERM)
            denied = true;
    } else {
        while (auto *ent = readdir(d)) {
            if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
                continue;
            std::string name = ent->d_name;
            if (!show_hidden_ && !name.empty() && name[0] == '.')
                continue;
            std::string full = dir + "/" + name;
            struct stat st {};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                names[name] = true;
        }
        closedir(d);
    }
    if (denied) {
        set_path_status(dir);
        set_work_state(WORK_ERR);
        return;
    }
    for (const auto &kv : db_dirs) {
        if (!show_hidden_ && !kv.first.empty() && kv.first[0] == '.')
            continue;
        names.emplace(kv.first, false);
    }
    for (const auto &kv : names) {
        auto *child = new Node();
        child->rel = node->rel.empty() ? kv.first : node->rel + "/" + kv.first;
        auto it = db_dirs.find(kv.first);
        child->folder_id = it == db_dirs.end() ? 0 : it->second;
        auto id = tree_->AppendItem(item, kv.first, 0, 0, child);
        tree_->SetItemHasChildren(id, true);
        if (!kv.second)
            tree_->SetItemTextColour(id, wxColour(140, 140, 140));
    }
}

void BrowserFrame::apply_view_mode() {
    if (!list_)
        return;
    long style = list_->GetWindowStyleFlag();
    style &= ~(wxLC_ICON | wxLC_SMALL_ICON | wxLC_LIST | wxLC_REPORT);
    switch (view_mode_) {
    case VIEW_ICON:
        /* Row-major: left-to-right, then next row. */
        style |= wxLC_ICON | wxLC_ALIGN_TOP;
        break;
    case VIEW_COMPACT:
        style |= wxLC_LIST;
        break;
    case VIEW_LIST:
    default:
        style |= wxLC_REPORT;
        break;
    }
    list_->SetWindowStyleFlag(style);
    list_->ClearAll();
    if (view_mode_ == VIEW_LIST) {
        list_->AppendColumn("Name", wxLIST_FORMAT_LEFT, 280);
        list_->AppendColumn("Status", wxLIST_FORMAT_LEFT, 90);
        list_->AppendColumn("Size", wxLIST_FORMAT_RIGHT, 90);
        list_->AppendColumn("SHA-1", wxLIST_FORMAT_LEFT, 340);
    }
    rebuild_list_icons();
    apply_list_font();
}

void BrowserFrame::apply_list_font() {
    if (!list_)
        return;
    wxFont f = list_->GetFont();
    int pt = 10 + zoom_level_;
    if (pt < 8)
        pt = 8;
    if (pt > 18)
        pt = 18;
    f.SetPointSize(pt);
    list_->SetFont(f);
}

void BrowserFrame::update_view_ui() {
    auto *mb = GetMenuBar();
    if (mb) {
        if (mb->FindItem(ID_VIEW_HIDDEN))
            mb->Check(ID_VIEW_HIDDEN, show_hidden_);
        if (mb->FindItem(ID_VIEW_THUMBS))
            mb->Check(ID_VIEW_THUMBS, show_thumbs_);
        if (mb->FindItem(ID_ARR_REVERSE))
            mb->Check(ID_ARR_REVERSE, arrange_rev_);
        int arr_id = ID_ARR_NAME;
        switch (arrange_) {
        case ARR_MANUAL:
            arr_id = ID_ARR_MANUAL;
            break;
        case ARR_SIZE:
            arr_id = ID_ARR_SIZE;
            break;
        case ARR_TYPE:
            arr_id = ID_ARR_TYPE;
            break;
        case ARR_MTIME:
            arr_id = ID_ARR_MTIME;
            break;
        case ARR_NAME:
        default:
            arr_id = ID_ARR_NAME;
            break;
        }
        if (mb->FindItem(arr_id))
            mb->Check(arr_id, true);
        int mode_id = ID_VIEW_LIST;
        if (view_mode_ == VIEW_ICON)
            mode_id = ID_VIEW_ICON;
        else if (view_mode_ == VIEW_COMPACT)
            mode_id = ID_VIEW_COMPACT;
        if (mb->FindItem(mode_id))
            mb->Check(mode_id, true);
    }
    if (view_tb_) {
        int tid = ID_VIEW_LIST;
        if (view_mode_ == VIEW_ICON)
            tid = ID_VIEW_ICON;
        else if (view_mode_ == VIEW_COMPACT)
            tid = ID_VIEW_COMPACT;
        view_tb_->ToggleTool(tid, true);
    }
}

void BrowserFrame::sort_list_rows() {
    auto type_key = [](const ListRow &r) -> std::string {
        if (r.is_dir)
            return {};
        auto dot = r.name.find_last_of('.');
        if (dot == std::string::npos)
            return {};
        std::string e = r.name.substr(dot + 1);
        for (char &c : e)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e;
    };
    auto cmp = [&](const ListRow &a, const ListRow &b) {
        if (a.is_dir != b.is_dir)
            return a.is_dir && !b.is_dir; /* dirs first */
        int c = 0;
        switch (arrange_) {
        case ARR_MANUAL:
            c = a.manual_order - b.manual_order;
            break;
        case ARR_SIZE: {
            auto sa = a.is_dir ? 0 : a.db.size;
            auto sb = b.is_dir ? 0 : b.db.size;
            c = (sa > sb) - (sa < sb);
            break;
        }
        case ARR_TYPE: {
            std::string ta = type_key(a), tb = type_key(b);
            c = ta.compare(tb);
            break;
        }
        case ARR_MTIME:
            c = (a.mtime > b.mtime) - (a.mtime < b.mtime);
            break;
        case ARR_NAME:
        default:
            c = a.name.compare(b.name);
            break;
        }
        if (c == 0)
            c = a.name.compare(b.name);
        return arrange_rev_ ? c > 0 : c < 0;
    };
    if (arrange_ == ARR_MANUAL && !arrange_rev_) {
        std::stable_sort(list_rows_.begin(), list_rows_.end(),
                         [](const ListRow &a, const ListRow &b) {
                             if (a.is_dir != b.is_dir)
                                 return a.is_dir && !b.is_dir;
                             return a.manual_order < b.manual_order;
                         });
    } else {
        std::stable_sort(list_rows_.begin(), list_rows_.end(), cmp);
    }
}

void BrowserFrame::fill_list_ctrl(bool enqueue_verify) {
    if (!list_)
        return;
    list_->DeleteAllItems();
    /* Rebuild thumbs into a fresh image list (folder/file bases at 0/1). */
    rebuild_list_icons();

    std::vector<VerifyJob> jobs;
    std::uint64_t gen = epoch_.load() + 1;
    epoch_.store(gen);
    const std::int64_t ttl =
        static_cast<std::int64_t>(std::max(1, auto_rewrite_months_) * 30) * 86400;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    long index = 0;
    for (auto &lr : list_rows_) {
        lr.icon_index = add_row_icon(lr);
        wxString status;
        if (lr.in_fs && lr.in_db) {
            status = lr.is_dir ? "" : "...";
        } else if (lr.in_fs) {
            status = "new";
        } else {
            status = "missing";
        }
        if (lr.in_fs && lr.in_db && !lr.is_dir && lr.db.verified_at > 0 &&
            now - lr.db.verified_at < ttl && lr.db.verify_status != VerifyStatus::Unknown) {
            status = lr.db.verify_status == VerifyStatus::Ok ? mark_ok() : mark_bad();
        }
        lr.status = status;

        long row = list_->InsertItem(index, lr.name, lr.icon_index);
        if (!lr.in_fs)
            list_->SetItemTextColour(row, wxColour(150, 150, 150));
        if (view_mode_ == VIEW_LIST) {
            list_->SetItem(row, 1, status);
            if (!lr.is_dir)
                list_->SetItem(row, 2, wxString::Format("%lld", static_cast<long long>(lr.db.size)));
            if (!lr.is_dir && lr.in_db)
                list_->SetItem(row, 3, sha1_hex(lr.db.sha1, lr.db.has_sha1));
        }

        bool recent = lr.db.verified_at > 0 && now - lr.db.verified_at < ttl &&
                      lr.db.verify_status != VerifyStatus::Unknown;
        if (enqueue_verify && lr.in_fs && lr.in_db && !lr.is_dir && !recent) {
            VerifyJob job;
            job.gen = gen;
            job.row = row;
            job.abs = lr.abs;
            job.db_path = lr.db_path;
            jobs.push_back(std::move(job));
        }
        ++index;
    }
    if (enqueue_verify)
        enqueue_verifies(gen, std::move(jobs));
    else
        enqueue_verifies(gen, {});
}

void BrowserFrame::refresh_current_folder() {
    auto *node = selected_node();
    if (node)
        show_folder(*node);
}

void BrowserFrame::show_folder(const Node &node) {
    list_rows_.clear();
    std::string dir = join_root(fs_root_, node.rel);
    const bool use_db = store_ && (node.folder_id != 0 || node.rel.empty());
    std::map<std::string, DirDbEntry> db;
    if (use_db) {
        for (const auto &e : store_->list_db_dir(node.folder_id))
            db[e.basename] = e;
    }

    struct Row {
        std::string name;
        bool is_dir = false;
        bool in_fs = false;
        bool in_db = false;
        DirDbEntry db;
        std::int64_t size = 0;
        std::int64_t mtime = 0;
        int order = 0;
    };
    std::map<std::string, Row> rows;
    int order = 0;

    bool denied = false;
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (errno == EACCES || errno == EPERM)
            denied = true;
    } else {
        while (auto *ent = readdir(d)) {
            if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
                continue;
            std::string name = ent->d_name;
            if (!show_hidden_ && !name.empty() && name[0] == '.')
                continue;
            std::string full = dir + "/" + name;
            struct stat st {};
            if (stat(full.c_str(), &st) != 0)
                continue;
            Row &r = rows[name];
            r.name = name;
            r.in_fs = true;
            r.is_dir = S_ISDIR(st.st_mode);
            r.mtime = static_cast<std::int64_t>(st.st_mtime);
            r.order = order++;
            if (!r.is_dir)
                r.size = st.st_size;
        }
        closedir(d);
    }

    if (denied) {
        if (list_)
            list_->DeleteAllItems();
        set_path_status(dir);
        set_work_state(WORK_ERR);
        return;
    }

    for (const auto &kv : db) {
        if (!show_hidden_ && !kv.first.empty() && kv.first[0] == '.')
            continue;
        Row &r = rows[kv.first];
        r.name = kv.first;
        r.in_db = true;
        r.db = kv.second;
        if (!r.in_fs) {
            r.is_dir = kv.second.directory;
            r.size = kv.second.size;
            r.order = order++;
        }
    }

    for (const auto &kv : rows) {
        const Row &r = kv.second;
        ListRow lr;
        lr.name = r.name;
        lr.is_dir = r.is_dir;
        lr.in_fs = r.in_fs;
        lr.in_db = r.in_db;
        lr.db = r.db;
        lr.abs = dir + "/" + r.name;
        lr.db_path = db_path_for(node.rel, r.name);
        lr.mtime = r.mtime;
        lr.manual_order = r.order;
        if (r.in_fs && !r.is_dir)
            lr.db.size = r.size;
        list_rows_.push_back(std::move(lr));
    }

    sort_list_rows();
    fill_list_ctrl(true);
    set_path_status(dir);
}

void BrowserFrame::enqueue_verifies(std::uint64_t gen, std::vector<VerifyJob> jobs) {
    const int n = static_cast<int>(jobs.size());
    {
        std::lock_guard<std::mutex> lock(qmu_);
        queue_.clear();
        pending_hashes_.store(0);
        QueueMsg cancel;
        cancel.kind = QueueMsg::Cancel;
        cancel.job.gen = gen;
        queue_.push_back(cancel);
        for (auto &j : jobs) {
            QueueMsg m;
            m.kind = QueueMsg::Verify;
            m.job = std::move(j);
            queue_.push_back(std::move(m));
        }
        if (n > 0)
            pending_hashes_.store(n);
        qcv_.notify_all();
    }
    if (n > 0)
        set_work_state(WORK_RUN);
    else if (work_state_.load() == WORK_RUN)
        set_work_state(WORK_IDLE);
    refresh_statusbar();
}

void BrowserFrame::worker_main() {
    for (;;) {
        QueueMsg msg;
        {
            std::unique_lock<std::mutex> lock(qmu_);
            qcv_.wait(lock, [this] { return stop_worker_ || !queue_.empty(); });
            if (stop_worker_ && queue_.empty())
                return;
            msg = std::move(queue_.front());
            queue_.pop_front();
        }
        if (msg.kind == QueueMsg::Cancel)
            continue;
        if (msg.job.gen != epoch_.load())
            continue;
        unsigned char dig[20];
        std::uint64_t nbytes = 0;
        bool hashed = hash_file(msg.job.abs, dig, &nbytes);
        if (hashed)
            note_scanned(nbytes);
        VerifyStatus st = VerifyStatus::Fail;
        if (hashed && store_) {
            ObjectRecord prev;
            bool have = store_->get_object(msg.job.db_path, prev);
            st = VerifyStatus::Ok;
            if (have && prev.has_sha1 && std::memcmp(prev.sha1, dig, 20) != 0)
                st = VerifyStatus::Fail;
            store_->bump_verify(msg.job.db_path, st, dig);
        } else if (store_) {
            store_->bump_verify(msg.job.db_path, VerifyStatus::Fail, nullptr);
        }
        const std::uint64_t gen = msg.job.gen;
        const long row = msg.job.row;
        const bool ok = st == VerifyStatus::Ok;
        wxString hex = hashed ? sha1_hex(dig, true) : wxString();
        int left = pending_hashes_.fetch_sub(1) - 1;
        CallAfter([this, gen, row, ok, hex, left] {
            if (gen != epoch_.load())
                return;
            if (row < 0 || row >= list_->GetItemCount())
                return;
            if (static_cast<size_t>(row) < list_rows_.size())
                list_rows_[static_cast<size_t>(row)].status = ok ? mark_ok() : mark_bad();
            if (view_mode_ == VIEW_LIST) {
                list_->SetItem(row, 1, ok ? mark_ok() : mark_bad());
                if (!hex.empty())
                    list_->SetItem(row, 3, hex);
            }
            refresh_statusbar();
            if (left <= 0 && work_state_.load() == WORK_RUN)
                set_work_state(WORK_IDLE);
        });
    }
}

void BrowserFrame::sync_rows(const std::vector<ListRow> &rows) {
    if (!store_) {
        alert_box("No database is open.", "Sync", wxOK | wxICON_INFORMATION, this);
        return;
    }
    set_work_state(WORK_SYNC);
    int added = 0, removed = 0, conflicts = 0, overwritten = 0, deleted = 0, ignored = 0;
    int sticky_choice = -1;
    for (const auto &r : rows) {
        if (r.is_dir) {
            if (r.in_db && !r.in_fs) {
                store_->delete_folder(r.db.id);
                ++removed;
            } else if (r.in_fs && !r.in_db) {
                auto *node = selected_node();
                std::int64_t parent = node ? node->folder_id : 0;
                store_->ensure_folder(parent, r.name);
                ++added;
            }
            continue;
        }
        if (r.in_fs && !r.in_db) {
            unsigned char dig[20];
            std::uint64_t nbytes = 0;
            if (!hash_file(r.abs, dig, &nbytes)) {
                alert_box(wxString::Format("Could not hash %s", r.abs), "Sync",
                             wxOK | wxICON_ERROR, this);
                continue;
            }
            note_scanned(nbytes);
            ObjectRecord o;
            o.path = r.db_path;
            o.size = r.db.size;
            std::memcpy(o.sha1, dig, 20);
            o.has_sha1 = true;
            o.verify_status = VerifyStatus::Ok;
            o.verified_at = static_cast<std::int64_t>(std::time(nullptr));
            store_->upsert_object(o);
            ++added;
            continue;
        }
        if (!r.in_fs && r.in_db) {
            store_->delete_file(r.db_path);
            ++removed;
            continue;
        }
        if (r.in_fs && r.in_db) {
            unsigned char dig[20];
            std::uint64_t nbytes = 0;
            if (!hash_file(r.abs, dig, &nbytes))
                continue;
            note_scanned(nbytes);
            bool conflict = r.db.has_sha1 && std::memcmp(r.db.sha1, dig, 20) != 0;
            if (!conflict && r.db.verify_status == VerifyStatus::Fail)
                conflict = true;
            if (!conflict) {
                store_->bump_verify(r.db_path, VerifyStatus::Ok, dig);
                continue;
            }
            ++conflicts;
            int choice = sticky_choice;
            if (choice < 0) {
                ConflictResult cr = ask_conflict(this, r.abs);
                choice = cr.choice;
                if (cr.apply_rest || cr.remember)
                    sticky_choice = choice;
            }
            if (choice == ConflictDelete) {
                if (::unlink(r.abs.c_str()) != 0) {
                    alert_box(wxString::Format("Could not delete %s: %s", r.abs, strerror(errno)),
                                 "Sync", wxOK | wxICON_ERROR, this);
                } else {
                    ++deleted;
                }
            } else if (choice == ConflictOverwrite) {
                store_->set_sha1(r.db_path, dig);
                store_->bump_verify(r.db_path, VerifyStatus::Ok, dig);
                ++overwritten;
            } else {
                store_->bump_verify(r.db_path, VerifyStatus::Fail, dig);
                ++ignored;
            }
        }
    }
    set_work_state(WORK_OK);
    set_path_status(wxString::Format("Sync: +%d new, -%d dangling, %d conflict(s)", added,
                                     removed, conflicts));
    (void)overwritten;
    (void)deleted;
    (void)ignored;
    refresh_statusbar();
    wxCommandEvent refresh_ev;
    on_refresh(refresh_ev);
}

void BrowserFrame::open_path(const std::string &path, bool pick_app) {
    if (path.empty())
        return;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        alert_box("Select a file on disk.", "Open", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (pick_app) {
        wxFileDialog dlg(this, "Open with...", "/usr/bin", "", "All files (*)|*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        wxString cmd = wxString::Format("%s %s", wxFileName(dlg.GetPath()).GetFullPath(),
                                        wxString::Format("'%s'", wxString::FromUTF8(path)));
        /* Prefer argv form when the program path has no spaces. */
        if (dlg.GetPath().Find(' ') == wxNOT_FOUND) {
            wxExecute(wxString::Format("%s %s", dlg.GetPath(),
                                       wxString::Format("\"%s\"", wxString::FromUTF8(path))),
                      wxEXEC_ASYNC);
        } else {
            wxExecute(cmd, wxEXEC_ASYNC);
        }
        return;
    }
    wxString ext = wxFileName(path).GetExt();
    (void)ext;
    /* Prefer xdg-open — avoids GIO/GVFS MIME lookup (unrelated to FUSE mounts). */
    wxExecute(wxString::Format("xdg-open \"%s\"", wxString::FromUTF8(path)), wxEXEC_ASYNC);
}

std::string BrowserFrame::selected_file_path() const {
    long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (i < 0 || static_cast<size_t>(i) >= list_rows_.size())
        return {};
    const ListRow &r = list_rows_[static_cast<size_t>(i)];
    if (r.is_dir || !r.in_fs)
        return {};
    return r.abs;
}

void BrowserFrame::on_tree_expanding(wxTreeEvent &ev) {
    /* Populate lazily; never touch selection here (avoids keyboard nav freezes). */
    populate_tree_children(ev.GetItem());
}

void BrowserFrame::schedule_show_folder(const Node &node) {
    pending_tree_rel_ = node.rel;
    if (pending_tree_show_)
        return;
    pending_tree_show_ = true;
    CallAfter([this]() {
        pending_tree_show_ = false;
        if (tree_sel_guard_)
            return;
        wxTreeItemId id = find_tree_by_rel(pending_tree_rel_);
        if (!id.IsOk())
            return;
        auto *node = dynamic_cast<Node *>(tree_->GetItemData(id));
        if (node)
            show_folder(*node);
    });
}

wxTreeItemId BrowserFrame::find_tree_by_rel(const std::string &rel) {
    if (rel.empty())
        return root_item_;
    wxTreeItemId cur = root_item_;
    std::string rest = rel;
    while (!rest.empty() && cur.IsOk()) {
        auto slash = rest.find('/');
        std::string part = slash == std::string::npos ? rest : rest.substr(0, slash);
        rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
        populate_tree_children(cur);
        wxTreeItemIdValue cookie;
        wxTreeItemId found;
        for (auto child = tree_->GetFirstChild(cur, cookie); child.IsOk();
             child = tree_->GetNextChild(cur, cookie)) {
            if (tree_->GetItemText(child) == part) {
                found = child;
                break;
            }
        }
        if (!found.IsOk())
            return {};
        cur = found;
    }
    return cur;
}

void BrowserFrame::restore_list_selection(const std::vector<std::string> &names) {
    if (!list_ || names.empty())
        return;
    for (const auto &n : names) {
        for (long i = 0; i < list_->GetItemCount(); ++i) {
            if (list_->GetItemText(i) == n) {
                list_->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
                break;
            }
        }
    }
}

void BrowserFrame::on_tree_sel(wxTreeEvent &ev) {
    if (tree_sel_guard_) {
        ev.Skip();
        return;
    }
    auto *node = dynamic_cast<Node *>(tree_->GetItemData(ev.GetItem()));
    if (node)
        schedule_show_folder(*node);
    ev.Skip();
}

void BrowserFrame::on_tree_menu(wxTreeEvent &ev) {
    tree_->SelectItem(ev.GetItem());
    wxMenu menu;
    menu.Append(ID_BR_SYNC, "Sync folder");
    menu.Append(ID_BR_REFRESH, "Refresh");
    PopupMenu(&menu);
}

void BrowserFrame::on_list_menu(wxListEvent &) {
    wxMenu menu;
    menu.Append(ID_BR_OPEN, "Open");
    menu.Append(ID_BR_OPEN_AS, "Open as...");
    menu.AppendSeparator();
    menu.Append(ID_BR_SYNC_SEL, "Sync selection");
    PopupMenu(&menu);
}

void BrowserFrame::on_list_activate(wxListEvent &ev) {
    long i = ev.GetIndex();
    if (i < 0 || static_cast<size_t>(i) >= list_rows_.size())
        return;
    const ListRow &r = list_rows_[static_cast<size_t>(i)];
    if (r.is_dir) {
        wxTreeItemId cur = tree_->GetSelection();
        if (!cur.IsOk())
            cur = root_item_;
        populate_tree_children(cur);
        wxTreeItemIdValue cookie;
        for (auto child = tree_->GetFirstChild(cur, cookie); child.IsOk();
             child = tree_->GetNextChild(cur, cookie)) {
            if (tree_->GetItemText(child) == r.name) {
                tree_sel_guard_ = true;
                tree_->SelectItem(child);
                tree_->EnsureVisible(child);
                tree_sel_guard_ = false;
                if (auto *node = dynamic_cast<Node *>(tree_->GetItemData(child)))
                    show_folder(*node);
                return;
            }
        }
        return;
    }
    if (r.in_fs)
        open_path(r.abs, false);
}

void BrowserFrame::on_sync_folder(wxCommandEvent &) {
    if (!list_rows_.empty())
        sync_rows(list_rows_);
}

void BrowserFrame::on_sync_selection(wxCommandEvent &) {
    std::vector<ListRow> sel;
    long i = -1;
    while ((i = list_->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
        if (static_cast<size_t>(i) < list_rows_.size())
            sel.push_back(list_rows_[static_cast<size_t>(i)]);
    }
    if (sel.empty()) {
        wxCommandEvent ev;
        on_sync_folder(ev);
        return;
    }
    sync_rows(sel);
}

void BrowserFrame::on_open(wxCommandEvent &) { open_path(selected_file_path(), false); }

void BrowserFrame::on_open_as(wxCommandEvent &) { open_path(selected_file_path(), true); }

void BrowserFrame::on_refresh(wxCommandEvent &) {
    std::string keep_rel;
    if (auto *node = selected_node())
        keep_rel = node->rel;
    std::vector<std::string> keep_names;
    if (list_) {
        long i = -1;
        while ((i = list_->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
            keep_names.push_back(list_->GetItemText(i).ToStdString());
    }

    /* Refresh folder contents without collapsing the whole tree. */
    wxTreeItemId cur = find_tree_by_rel(keep_rel);
    if (!cur.IsOk())
        cur = root_item_;

    tree_sel_guard_ = true;
    /* Rebuild children of the current node only. */
    tree_->DeleteChildren(cur);
    populate_tree_children(cur);
    if (keep_rel.empty()) {
        /* Also refresh root's open branches lightly — selection path only. */
    }
    tree_->SelectItem(cur);
    tree_->EnsureVisible(cur);
    tree_sel_guard_ = false;

    if (auto *node = dynamic_cast<Node *>(tree_->GetItemData(cur)))
        show_folder(*node);
    restore_list_selection(keep_names);
}

void BrowserFrame::on_up(wxCommandEvent &) {
    wxTreeItemId cur = tree_->GetSelection();
    if (!cur.IsOk() || cur == root_item_)
        return;
    wxTreeItemId parent = tree_->GetItemParent(cur);
    if (parent.IsOk())
        tree_->SelectItem(parent);
}

void BrowserFrame::on_close(wxCommandEvent &) { Close(true); }

void BrowserFrame::set_zoom_level(int level) {
    if (level < kZoomMin)
        level = kZoomMin;
    if (level > kZoomMax)
        level = kZoomMax;
    if (level == zoom_level_)
        return;
    zoom_level_ = level;
    apply_list_font();
    sort_list_rows();
    fill_list_ctrl(false);
    save_settings();
}

void BrowserFrame::set_view_mode(ViewMode mode) {
    if (mode == view_mode_) {
        update_view_ui();
        return;
    }
    view_mode_ = mode;
    apply_view_mode();
    sort_list_rows();
    fill_list_ctrl(false);
    update_view_ui();
    save_settings();
}

void BrowserFrame::on_list_wheel(wxMouseEvent &ev) {
    if (ev.ControlDown()) {
        if (ev.GetWheelRotation() > 0)
            set_zoom_level(zoom_level_ + 1);
        else if (ev.GetWheelRotation() < 0)
            set_zoom_level(zoom_level_ - 1);
        return;
    }
    ev.Skip();
}

void BrowserFrame::on_view_reset(wxCommandEvent &) {
    show_hidden_ = false;
    show_thumbs_ = false;
    arrange_ = ARR_NAME;
    arrange_rev_ = false;
    zoom_level_ = 0;
    view_mode_ = VIEW_LIST;
    apply_view_mode();
    update_view_ui();
    /* Rebuild tree children when hidden filter changes. */
    tree_->DeleteChildren(root_item_);
    populate_tree_children(root_item_);
    tree_->Expand(root_item_);
    refresh_current_folder();
    save_settings();
}

void BrowserFrame::on_view_hidden(wxCommandEvent &ev) {
    show_hidden_ = ev.IsChecked();
    tree_->DeleteChildren(root_item_);
    populate_tree_children(root_item_);
    tree_->Expand(root_item_);
    refresh_current_folder();
    save_settings();
}

void BrowserFrame::on_view_thumbs(wxCommandEvent &ev) {
    show_thumbs_ = ev.IsChecked();
    fill_list_ctrl(false);
    save_settings();
}

void BrowserFrame::on_view_arrange(wxCommandEvent &ev) {
    switch (ev.GetId()) {
    case ID_ARR_MANUAL:
        arrange_ = ARR_MANUAL;
        break;
    case ID_ARR_SIZE:
        arrange_ = ARR_SIZE;
        break;
    case ID_ARR_TYPE:
        arrange_ = ARR_TYPE;
        break;
    case ID_ARR_MTIME:
        arrange_ = ARR_MTIME;
        break;
    case ID_ARR_NAME:
    default:
        arrange_ = ARR_NAME;
        break;
    }
    sort_list_rows();
    fill_list_ctrl(false);
    save_settings();
}

void BrowserFrame::on_view_reverse(wxCommandEvent &ev) {
    arrange_rev_ = ev.IsChecked();
    sort_list_rows();
    fill_list_ctrl(false);
    save_settings();
}

void BrowserFrame::on_view_zoom_in(wxCommandEvent &) { set_zoom_level(zoom_level_ + 1); }

void BrowserFrame::on_view_zoom_out(wxCommandEvent &) { set_zoom_level(zoom_level_ - 1); }

void BrowserFrame::on_view_zoom_normal(wxCommandEvent &) { set_zoom_level(0); }

void BrowserFrame::on_view_mode(wxCommandEvent &ev) {
    ViewMode mode = VIEW_LIST;
    switch (ev.GetId()) {
    case ID_VIEW_ICON:
        mode = VIEW_ICON;
        break;
    case ID_VIEW_COMPACT:
        mode = VIEW_COMPACT;
        break;
    case ID_VIEW_LIST:
    default:
        mode = VIEW_LIST;
        break;
    }
    set_view_mode(mode);
}

BrowserSettings BrowserFrame::current_settings() const {
    BrowserSettings s;
    s.view_mode = static_cast<int>(view_mode_);
    s.arrange = static_cast<int>(arrange_);
    s.arrange_rev = arrange_rev_;
    s.show_hidden = show_hidden_;
    s.show_thumbs = show_thumbs_;
    s.zoom_level = zoom_level_;
    s.auto_rewrite_months = auto_rewrite_months_;
    return s;
}

void BrowserFrame::load_settings() {
    BrowserSettings s = load_browser_settings();
    if (s.view_mode < 0 || s.view_mode > 2)
        s.view_mode = 1;
    if (s.arrange < 0 || s.arrange > 4)
        s.arrange = 1;
    view_mode_ = static_cast<ViewMode>(s.view_mode);
    arrange_ = static_cast<ArrangeBy>(s.arrange);
    arrange_rev_ = s.arrange_rev;
    show_hidden_ = s.show_hidden;
    show_thumbs_ = s.show_thumbs;
    zoom_level_ = s.zoom_level;
    if (zoom_level_ < kZoomMin)
        zoom_level_ = kZoomMin;
    if (zoom_level_ > kZoomMax)
        zoom_level_ = kZoomMax;
    auto_rewrite_months_ = s.auto_rewrite_months;
    if (auto_rewrite_months_ < 1)
        auto_rewrite_months_ = 1;
    if (auto_rewrite_months_ > 120)
        auto_rewrite_months_ = 120;
}

void BrowserFrame::save_settings() { save_browser_settings(current_settings()); }

void BrowserFrame::apply_auto_rewrite_months(int months) {
    if (months < 1)
        months = 1;
    if (months > 120)
        months = 120;
    auto_rewrite_months_ = months;
    if (store_)
        store_->set_sha1_valid_days(months * 30);
}

void BrowserFrame::on_prefs(wxCommandEvent &) {
    wxDialog dlg(this, wxID_ANY, "Browser Preferences", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);

    auto add_label = [&](const wxString &text) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
    };

    add_label("Auto rewrite files for (months)");
    auto *spin_months = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                       wxSP_ARROW_KEYS, 1, 120, auto_rewrite_months_);
    grid->Add(spin_months, 0, wxEXPAND);

    add_label("Default view");
    auto *choice_view = new wxChoice(&dlg, wxID_ANY);
    choice_view->Append("Icon");
    choice_view->Append("List");
    choice_view->Append("Compact");
    choice_view->SetSelection(static_cast<int>(view_mode_));
    grid->Add(choice_view, 0, wxEXPAND);

    add_label("Arrange by");
    auto *choice_arr = new wxChoice(&dlg, wxID_ANY);
    choice_arr->Append("Manually");
    choice_arr->Append("Name");
    choice_arr->Append("Size");
    choice_arr->Append("Type");
    choice_arr->Append("Modification date");
    choice_arr->SetSelection(static_cast<int>(arrange_));
    grid->Add(choice_arr, 0, wxEXPAND);

    add_label("");
    auto *chk_rev = new wxCheckBox(&dlg, wxID_ANY, "Reversed order");
    chk_rev->SetValue(arrange_rev_);
    grid->Add(chk_rev, 0, wxEXPAND);

    add_label("");
    auto *chk_hidden = new wxCheckBox(&dlg, wxID_ANY, "Show hidden files");
    chk_hidden->SetValue(show_hidden_);
    grid->Add(chk_hidden, 0, wxEXPAND);

    add_label("");
    auto *chk_thumbs = new wxCheckBox(&dlg, wxID_ANY, "Show thumbnails");
    chk_thumbs->SetValue(show_thumbs_);
    grid->Add(chk_thumbs, 0, wxEXPAND);

    add_label("Zoom level");
    auto *spin_zoom = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                     wxSP_ARROW_KEYS, kZoomMin, kZoomMax, zoom_level_);
    grid->Add(spin_zoom, 0, wxEXPAND);

    root->Add(grid, 0, wxEXPAND | wxALL, 12);
    root->Add(new wxStaticText(&dlg, wxID_ANY,
                               "Files verified within the auto-rewrite window are not "
                               "re-hashed until the window expires.\n"
                               "Settings are saved under ~/.config/sdtouch/."),
              0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    root->Add(dlg.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
    dlg.SetSizerAndFit(root);

    if (dlg.ShowModal() != wxID_OK)
        return;

    bool hidden_changed = chk_hidden->GetValue() != show_hidden_;
    apply_auto_rewrite_months(spin_months->GetValue());
    arrange_ = static_cast<ArrangeBy>(choice_arr->GetSelection());
    arrange_rev_ = chk_rev->GetValue();
    show_hidden_ = chk_hidden->GetValue();
    show_thumbs_ = chk_thumbs->GetValue();
    zoom_level_ = spin_zoom->GetValue();
    view_mode_ = static_cast<ViewMode>(choice_view->GetSelection());
    apply_view_mode();
    apply_list_font();
    update_view_ui();
    if (hidden_changed) {
        tree_->DeleteChildren(root_item_);
        populate_tree_children(root_item_);
        tree_->Expand(root_item_);
    }
    sort_list_rows();
    fill_list_ctrl(true);
    save_settings();
}

} /* namespace sdmsg */
