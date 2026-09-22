/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/grid_panel.hpp"

#include <cmath>
#include <wx/dcclient.h>

namespace sdmsg {

GridPanel::GridPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 220)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &GridPanel::on_paint, this);
}

void GridPanel::update_cells(const std::vector<CellStatus> &cells) {
    cells_ = cells;
    Refresh(false);
}

void GridPanel::on_paint(wxPaintEvent &) {
    wxPaintDC dc(this);
    wxSize sz = GetClientSize();
    dc.SetBrush(wxBrush(wxColour(40, 40, 40)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    if (cells_.empty())
        return;

    int n = static_cast<int>(cells_.size());
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    if (cols < 1)
        cols = 1;
    int rows = (n + cols - 1) / cols;
    int cw = std::max(2, sz.x / cols);
    int ch = std::max(2, sz.y / rows);

    for (int i = 0; i < n; ++i) {
        int x = (i % cols) * cw;
        int y = (i / cols) * ch;
        wxColour c;
        switch (cells_[static_cast<size_t>(i)]) {
        case CellStatus::Pending:
            c = wxColour(90, 90, 90);
            break;
        case CellStatus::Ok:
            c = wxColour(60, 160, 80);
            break;
        case CellStatus::ReadErr:
            c = wxColour(200, 60, 60);
            break;
        case CellStatus::WriteErr:
            c = wxColour(220, 140, 40);
            break;
        }
        dc.SetBrush(wxBrush(c));
        dc.DrawRectangle(x + 1, y + 1, cw - 2, ch - 2);
    }
}

} /* namespace sdmsg */
