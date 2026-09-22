/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/massage.hpp"
#include "gui/grid_panel.hpp"
#include "gui/log_panel.hpp"

#include <memory>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/button.h>

namespace sdmsg {

class MainFrame : public wxFrame {
public:
    explicit MainFrame(std::shared_ptr<MassageEngine> engine);

private:
    std::shared_ptr<MassageEngine> engine_;
    wxGauge *gauge_ = nullptr;
    wxStaticText *status_ = nullptr;
    GridPanel *grid_ = nullptr;
    LogPanel *log_ = nullptr;
    wxButton *pause_btn_ = nullptr;
    wxTimer timer_;
    bool paused_ = false;
    bool offered_bad_ = false;

    void on_timer(wxTimerEvent &);
    void on_pause(wxCommandEvent &);
    void on_stop(wxCommandEvent &);
    void maybe_offer_badblocks();
};

} /* namespace sdmsg */
