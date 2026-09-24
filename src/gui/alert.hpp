/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdio>
#include <wx/msgdlg.h>
#include <wx/string.h>
#include <wx/window.h>

namespace sdmsg {

/*
 * Drop-in for wxMessageBox: same argument order, also prints to stderr.
 * Use for every warning/error/info dialog the user sees.
 */
inline int alert_box(const wxString &message, const wxString &caption = wxMessageBoxCaptionStr,
                     long style = wxOK | wxCENTRE, wxWindow *parent = nullptr) {
    const char *tag = "info";
    if (style & wxICON_ERROR)
        tag = "error";
    else if (style & wxICON_WARNING)
        tag = "warn";
    else if (style & wxICON_QUESTION)
        tag = "ask";
    std::fprintf(stderr, "sdmsg: %s: [%s] %s\n", tag, caption.utf8_str().data(),
                 message.utf8_str().data());
    std::fflush(stderr);
    return wxMessageBox(message, caption, style, parent);
}

} /* namespace sdmsg */
