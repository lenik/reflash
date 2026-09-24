/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/progress.hpp"

#include <vector>
#include <wx/panel.h>

namespace reflash {

class GridPanel : public wxPanel {
public:
    explicit GridPanel(wxWindow *parent);
    void update_cells(const std::vector<CellStatus> &cells);
    void clear();
    void reset_pending();
    size_t cell_count() const { return cells_.size(); }
    size_t count_status(CellStatus st) const;

private:
    std::vector<CellStatus> cells_;
    void on_paint(wxPaintEvent &);
};

} /* namespace reflash */
