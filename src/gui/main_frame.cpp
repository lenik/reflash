/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/main_frame.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/panel.h>

namespace sdmsg {

static wxString format_rate(double bps) {
    if (bps < 1024)
        return wxString::Format("%.0f B/s", bps);
    if (bps < 1024 * 1024)
        return wxString::Format("%.1f KiB/s", bps / 1024.0);
    return wxString::Format("%.2f MiB/s", bps / (1024.0 * 1024.0));
}

static wxString format_eta(double sec) {
    if (sec < 0)
        return "ETA --";
    int s = static_cast<int>(sec);
    int h = s / 3600;
    int m = (s % 3600) / 60;
    s %= 60;
    if (h > 0)
        return wxString::Format("ETA %d:%02d:%02d", h, m, s);
    return wxString::Format("ETA %d:%02d", m, s);
}

MainFrame::MainFrame(std::shared_ptr<MassageEngine> engine)
    : wxFrame(nullptr, wxID_ANY, "sdmsg", wxDefaultPosition, wxSize(900, 600)),
      engine_(std::move(engine)), timer_(this) {
    auto *panel = new wxPanel(this);
    auto *root = new wxBoxSizer(wxVERTICAL);

    gauge_ = new wxGauge(panel, wxID_ANY, 1000);
    status_ = new wxStaticText(panel, wxID_ANY, "Starting...");
    grid_ = new GridPanel(panel);
    log_ = new LogPanel(panel);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    pause_btn_ = new wxButton(panel, wxID_ANY, "Pause");
    auto *stop_btn = new wxButton(panel, wxID_ANY, "Stop");
    btns->Add(pause_btn_, 0, wxALL, 4);
    btns->Add(stop_btn, 0, wxALL, 4);

    root->Add(status_, 0, wxEXPAND | wxALL, 6);
    root->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);
    root->Add(btns, 0, wxLEFT | wxRIGHT, 2);
    root->Add(grid_, 1, wxEXPAND | wxALL, 6);
    root->Add(log_, 0, wxEXPAND | wxALL, 6);
    panel->SetSizer(root);

    pause_btn_->Bind(wxEVT_BUTTON, &MainFrame::on_pause, this);
    stop_btn->Bind(wxEVT_BUTTON, &MainFrame::on_stop, this);
    Bind(wxEVT_TIMER, &MainFrame::on_timer, this);
    timer_.Start(200);
}

void MainFrame::on_pause(wxCommandEvent &) {
    if (!paused_) {
        engine_->pause().pause();
        engine_->progress().set_paused(true);
        pause_btn_->SetLabel("Resume");
        paused_ = true;
    } else {
        engine_->pause().resume();
        engine_->progress().set_paused(false);
        pause_btn_->SetLabel("Pause");
        paused_ = false;
    }
}

void MainFrame::on_stop(wxCommandEvent &) {
    engine_->pause().request_stop();
    engine_->pause().resume();
}

void MainFrame::on_timer(wxTimerEvent &) {
    auto snap = engine_->progress().snapshot();
    if (snap.bytes_total > 0) {
        int v = static_cast<int>((snap.bytes_done * 1000) / snap.bytes_total);
        gauge_->SetValue(std::min(1000, v));
    }
    double pct =
        snap.bytes_total ? (100.0 * static_cast<double>(snap.bytes_done) / snap.bytes_total) : 0.0;
    status_->SetLabel(wxString::Format(
        "%s | %.1f%% | %s | %s | cells=%zu (shift=%u)%s", snap.phase, pct,
        format_rate(snap.bytes_per_sec), format_eta(snap.eta_seconds), snap.cells.size(),
        snap.cell_shift, snap.paused ? " | PAUSED" : ""));
    grid_->update_cells(snap.cells);

    for (const auto &e : engine_->progress().take_logs()) {
        log_->append(wxString::Format("[%llu +%llu] %s",
                                      static_cast<unsigned long long>(e.offset),
                                      static_cast<unsigned long long>(e.length), e.message));
    }

    if (snap.finished) {
        timer_.Stop();
        maybe_offer_badblocks();
    }
}

void MainFrame::maybe_offer_badblocks() {
    if (offered_bad_)
        return;
    offered_bad_ = true;
    auto bad = engine_->store().bad_extents();
    if (bad.empty()) {
        wxMessageBox("Run finished with no bad extents recorded.", "sdmsg", wxOK | wxICON_INFORMATION,
                     this);
        return;
    }
    int ans = wxMessageBox(
        wxString::Format(
            "Recorded %zu bad extent(s).\nExport a badblocks-compatible list of block numbers?",
            bad.size()),
        "sdmsg", wxYES_NO | wxICON_WARNING, this);
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
    wxMessageBox("Wrote bad block list. For ext filesystems you may pass it to e2fsck -l.\n"
                 "Do not mark blocks without understanding the filesystem consequences.",
                 "sdmsg", wxOK | wxICON_INFORMATION, this);
}

} /* namespace sdmsg */
