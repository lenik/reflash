/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui/app.hpp"
#include "gui/main_frame.hpp"

#include <cstdlib>
#include <wx/image.h>
#include <wx/wx.h>

namespace reflash {

Options g_gui_opts;

static void ensure_image_handlers() {
    auto add = [](wxImageHandler *h, wxBitmapType t) {
        if (!wxImage::FindHandler(t))
            wxImage::AddHandler(h);
        else
            delete h;
    };
    add(new wxPNGHandler(), wxBITMAP_TYPE_PNG);
    add(new wxJPEGHandler(), wxBITMAP_TYPE_JPEG);
    add(new wxGIFHandler(), wxBITMAP_TYPE_GIF);
    add(new wxBMPHandler(), wxBITMAP_TYPE_BMP);
    add(new wxXPMHandler(), wxBITMAP_TYPE_XPM);
    add(new wxTIFFHandler(), wxBITMAP_TYPE_TIFF);
}

class SdmsgApp : public wxApp {
public:
    bool OnInit() override {
        ensure_image_handlers();
        auto *frame = new MainFrame(g_gui_opts);
        frame->Show(true);
        return true;
    }
};

int run_gui(Options opts, int &argc, char **argv) {
    /* Avoid dbind accessibility-bus timeout spam when AT-SPI is broken/slow. */
    setenv("NO_AT_BRIDGE", "1", 0);
    g_gui_opts = std::move(opts);
    wxApp::SetInstance(new SdmsgApp());
    return wxEntry(argc, argv);
}

} /* namespace reflash */

wxIMPLEMENT_APP_NO_MAIN(reflash::SdmsgApp);
