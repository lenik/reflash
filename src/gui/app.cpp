/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/app.hpp"
#include "gui/main_frame.hpp"

#include <wx/wx.h>

namespace sdmsg {

std::shared_ptr<MassageEngine> g_gui_engine;

class SdmsgApp : public wxApp {
public:
    bool OnInit() override {
        if (!g_gui_engine)
            return false;
        wxImage::AddHandler(new wxPNGHandler());
        auto *frame = new MainFrame(g_gui_engine);
        frame->Show(true);
        std::string err;
        if (!g_gui_engine->start(&err)) {
            wxMessageBox(err, "sdmsg", wxOK | wxICON_ERROR);
            return false;
        }
        return true;
    }
};

int run_gui(std::shared_ptr<MassageEngine> engine, int &argc, char **argv) {
    g_gui_engine = std::move(engine);
    wxApp::SetInstance(new SdmsgApp());
    return wxEntry(argc, argv);
}

} /* namespace sdmsg */

wxIMPLEMENT_APP_NO_MAIN(sdmsg::SdmsgApp);
