/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/grid_panel.hpp"

#include <algorithm>
#include <wx/dcclient.h>

namespace reflash {

GridPanel::GridPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 220)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &GridPanel::on_paint, this);
}

size_t GridPanel::count_status(CellStatus st) const {
    size_t n = 0;
    for (CellStatus c : cells_) {
        if (c == st)
            ++n;
    }
    return n;
}

void GridPanel::clear() {
    cells_.clear();
    Refresh(false);
}

void GridPanel::reset_pending() {
    for (CellStatus &c : cells_)
        c = CellStatus::Pending;
    Refresh(false);
    Update();
}

void GridPanel::update_cells(const std::vector<CellStatus> &cells) {
    /* Keep the last scan map if a later phase publishes an empty grid. */
    if (cells.empty() && !cells_.empty())
        return;
    cells_ = cells;
    Refresh(false);
}

void GridPanel::on_paint(wxPaintEvent &) {
    wxPaintDC dc(this);
    wxSize sz = GetClientSize();
    dc.SetBrush(wxBrush(wxColour(40, 40, 40)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    if (cells_.empty() || sz.x < 2 || sz.y < 2)
        return;

    int n = static_cast<int>(cells_.size());
    int best_cols = 1;
    int best_side = 1;
    for (int cols = 1; cols <= n; ++cols) {
        int rows = (n + cols - 1) / cols;
        int side = std::min(sz.x / cols, sz.y / rows);
        if (side > best_side) {
            best_side = side;
            best_cols = cols;
        }
    }
    int rows = (n + best_cols - 1) / best_cols;
    int grid_w = best_cols * best_side;
    int grid_h = rows * best_side;
    int ox = (sz.x - grid_w) / 2;
    int oy = (sz.y - grid_h) / 2;
    int gap = best_side > 6 ? 1 : 0;
    int draw = std::max(1, best_side - gap);

    for (int i = 0; i < n; ++i) {
        int x = ox + (i % best_cols) * best_side;
        int y = oy + (i / best_cols) * best_side;
        wxColour c;
        switch (cells_[static_cast<size_t>(i)]) {
        case CellStatus::Pending:
            c = wxColour(90, 90, 90);
            break;
        case CellStatus::Cached:
            c = wxColour(50, 110, 210);
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
        dc.DrawRectangle(x, y, draw, draw);
    }
}

} /* namespace reflash */
