/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/progress.hpp"

#include <vector>
#include <wx/listctrl.h>
#include <wx/panel.h>

namespace reflash {

class LogPanel : public wxPanel {
public:
    explicit LogPanel(wxWindow *parent);

    void append(const LogEntry &e);
    void clear();
    void set_min_level(LogLevel level);
    size_t row_count() const { return rows_.size(); }

private:
    struct Row {
        LogLevel level = LogLevel::Info;
        wxString time;
        wxString level_name;
        wxString message;
        int image = 1;
    };

    wxListCtrl *list_ = nullptr;
    std::vector<Row> rows_;
    LogLevel min_ = LogLevel::Info;

    void refill();
};

} /* namespace reflash */
