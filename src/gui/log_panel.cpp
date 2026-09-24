/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/log_panel.hpp"

#include <ctime>
#include <wx/artprov.h>
#include <wx/datetime.h>
#include <wx/imaglist.h>
#include <wx/sizer.h>

namespace reflash {

namespace {

wxString level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warn:
        return "Warn";
    case LogLevel::Error:
        return "Error";
    }
    return "Info";
}

int level_image(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug:
        return 0;
    case LogLevel::Info:
        return 1;
    case LogLevel::Warn:
        return 2;
    case LogLevel::Error:
        return 3;
    }
    return 1;
}

wxString format_time(std::int64_t sec) {
    if (sec <= 0)
        sec = static_cast<std::int64_t>(std::time(nullptr));
    wxDateTime dt(static_cast<time_t>(sec));
    return dt.Format("%H:%M:%S");
}

} /* namespace */

LogPanel::LogPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY) {
    auto *root = new wxBoxSizer(wxVERTICAL);
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 160),
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
    auto *images = new wxImageList(16, 16, true);
    images->Add(wxArtProvider::GetBitmap(wxART_TIP, wxART_MENU, wxSize(16, 16)));
    images->Add(wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_MENU, wxSize(16, 16)));
    images->Add(wxArtProvider::GetBitmap(wxART_WARNING, wxART_MENU, wxSize(16, 16)));
    images->Add(wxArtProvider::GetBitmap(wxART_ERROR, wxART_MENU, wxSize(16, 16)));
    list_->AssignImageList(images, wxIMAGE_LIST_SMALL);
    list_->AppendColumn("", wxLIST_FORMAT_CENTER, 28);
    list_->AppendColumn("Time", wxLIST_FORMAT_LEFT, 84);
    list_->AppendColumn("Level", wxLIST_FORMAT_LEFT, 64);
    list_->AppendColumn("Message", wxLIST_FORMAT_LEFT, 520);
    root->Add(list_, 1, wxEXPAND | wxALL, 4);
    SetSizer(root);
}

void LogPanel::append(const LogEntry &e) {
    Row row;
    row.level = e.level;
    row.time = format_time(e.time_sec);
    row.level_name = level_name(e.level);
    row.message = e.message;
    if (e.length)
        row.message += wxString::Format("  @%llu+%llu", static_cast<unsigned long long>(e.offset),
                                        static_cast<unsigned long long>(e.length));
    row.image = level_image(e.level);
    rows_.push_back(row);
    if (row.level < min_)
        return;
    long i = list_->InsertItem(list_->GetItemCount(), "", row.image);
    list_->SetItem(i, 1, row.time);
    list_->SetItem(i, 2, row.level_name);
    list_->SetItem(i, 3, row.message);
    list_->EnsureVisible(i);
}

void LogPanel::clear() {
    rows_.clear();
    if (list_)
        list_->DeleteAllItems();
}

void LogPanel::set_min_level(LogLevel level) {
    min_ = level;
    refill();
}

void LogPanel::refill() {
    list_->DeleteAllItems();
    for (const auto &row : rows_) {
        if (row.level < min_)
            continue;
        long i = list_->InsertItem(list_->GetItemCount(), "", row.image);
        list_->SetItem(i, 1, row.time);
        list_->SetItem(i, 2, row.level_name);
        list_->SetItem(i, 3, row.message);
    }
}

} /* namespace reflash */
