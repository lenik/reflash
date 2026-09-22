/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "engine/progress.hpp"

#include <vector>
#include <wx/panel.h>

namespace sdmsg {

class GridPanel : public wxPanel {
public:
    explicit GridPanel(wxWindow *parent);
    void update_cells(const std::vector<CellStatus> &cells);

private:
    std::vector<CellStatus> cells_;
    void on_paint(wxPaintEvent &);
};

} /* namespace sdmsg */
