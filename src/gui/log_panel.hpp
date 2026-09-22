/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <wx/textctrl.h>

namespace sdmsg {

class LogPanel : public wxTextCtrl {
public:
    explicit LogPanel(wxWindow *parent)
        : wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 140),
                     wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2) {}

    void append(const wxString &line) {
        AppendText(line + "\n");
    }
};

} /* namespace sdmsg */
