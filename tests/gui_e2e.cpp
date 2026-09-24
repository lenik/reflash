/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GUI end-to-end: a throwaway file (never a real disk) is opened, Start is
 * run from the Edit menu, and a second frame must reopen that file.
 */

#include "gui/main_frame.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <wx/app.h>
#include <wx/menu.h>
#include <wx/toolbar.h>
#include <wx/utils.h>

namespace {

int g_code = 1;

std::string make_fixture(std::string *session_out, std::string *err) {
    char dir[] = "/tmp/reflash-e2e-XXXXXX";
    if (!mkdtemp(dir)) {
        *err = "mkdtemp failed";
        return {};
    }
    std::string image = std::string(dir) + "/testdrive.bin";
    std::vector<char> buf(256 * 1024);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<char>(i * 17u + 3u);
    std::ofstream out(image, std::ios::binary);
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!out) {
        *err = "could not write " + image;
        return {};
    }
    *session_out = std::string(dir) + "/gui.ini";
    return image;
}

bool fail(const std::string &msg) {
    fprintf(stderr, "gui_e2e: %s\n", msg.c_str());
    return false;
}

bool scenario() {
    std::string err, session, image = make_fixture(&session, &err);
    if (image.empty())
        return fail(err);
    setenv("REFLASH_SESSION", session.c_str(), 1);

    reflash::Options opts;
    auto *frame = new reflash::MainFrame(opts);
    frame->Show(true);
    frame->open_target(image);

    auto *bar = frame->GetMenuBar();
    int start_id = bar ? bar->FindMenuItem("&Edit", "&Start\tCtrl+R") : wxNOT_FOUND;
    int pause_id = bar ? bar->FindMenuItem("&Edit", "&Pause\tCtrl+P") : wxNOT_FOUND;
    int stop_id = bar ? bar->FindMenuItem("&Edit", "S&top\tCtrl+.") : wxNOT_FOUND;
    int dry_id = bar ? bar->FindMenuItem("&Edit", "&Dry-run\tCtrl+3") : wxNOT_FOUND;
    int image_id = bar ? bar->FindMenuItem("&File", "&Image File...\tCtrl+O") : wxNOT_FOUND;
    int manifest_id =
        bar ? bar->FindMenuItem("&Manifest", "&Use external SQLite...\tCtrl+D") : wxNOT_FOUND;
    if (start_id == wxNOT_FOUND || pause_id == wxNOT_FOUND || stop_id == wxNOT_FOUND)
        return fail("Edit menu is missing Start, Pause, or Stop");
    if (dry_id == wxNOT_FOUND)
        return fail("Edit menu is missing Dry-run");
    if (image_id == wxNOT_FOUND)
        return fail("File menu is missing Image File");
    if (manifest_id == wxNOT_FOUND)
        return fail("Manifest menu is missing Use external SQLite");

    auto *tools = frame->GetToolBar();
    if (!tools || !tools->FindById(start_id) || !tools->FindById(pause_id) || !tools->FindById(stop_id))
        return fail("toolbar is missing Start, Pause, or Stop");
    /* Drive / Manifest are check tools with their own IDs */
    int drive_tool = -1;
    int manifest_tool = -1;
    for (int i = 0; i < tools->GetToolsCount(); ++i) {
        auto *t = tools->GetToolByPos(i);
        if (!t)
            continue;
        if (t->GetLabel() == "Drive")
            drive_tool = t->GetId();
        if (t->GetLabel() == "Manifest")
            manifest_tool = t->GetId();
    }
    if (drive_tool < 0 || manifest_tool < 0)
        return fail("toolbar is missing Drive or Manifest");
    if (!tools->GetToolState(drive_tool))
        return fail("Drive tool is not pressed after opening a target");
    if (!tools->GetToolEnabled(start_id))
        return fail("Start tool is disabled after opening the test file");

    auto wait_done = [&](reflash::MainFrame *f, wxString *status_out) -> bool {
        for (int i = 0; i < 400; ++i) {
            wxYield();
            ::wxMilliSleep(25);
            if (!f->GetStatusBar())
                return false;
            *status_out = f->GetStatusBar()->GetStatusText(0);
            if (status_out->Contains("Finished") || status_out->Contains("Failed") ||
                status_out->Contains("Stopped"))
                return true;
        }
        return false;
    };

    wxCommandEvent ev(wxEVT_MENU, start_id);
    ev.SetEventObject(frame);
    frame->ProcessWindowEvent(ev);

    wxString status;
    if (!wait_done(frame, &status))
        return fail(std::string("status stayed at: ") + status.ToStdString());
    if (status.Contains("Starting"))
        return fail("status still says Starting");
    if (status != "Finished")
        return fail(std::string("expected Finished, got: ") + status.ToStdString());
    if (frame->grid_cells() == 0)
        return fail("scan grid is empty");
    if (frame->grid_status_count(reflash::CellStatus::Ok) == 0)
        return fail("scan grid has no completed cells");
    if (frame->log_count() == 0)
        return fail("log is empty");

    unsigned resets = frame->grid_resets();
    frame->ProcessWindowEvent(ev);
    if (!wait_done(frame, &status) || status != "Finished")
        return fail(std::string("restart status: ") + status.ToStdString());
    if (frame->grid_resets() <= resets)
        return fail("restart did not reset the grid");
    if (frame->grid_status_count(reflash::CellStatus::Ok) == 0)
        return fail("restart grid has no completed cells");

    int close_id = bar->FindMenuItem("&File", "&Close\tCtrl+W");
    if (close_id == wxNOT_FOUND)
        return fail("File menu is missing Close");
    wxCommandEvent close_ev(wxEVT_MENU, close_id);
    close_ev.SetEventObject(frame);
    frame->ProcessWindowEvent(close_ev);
    if (frame->grid_cells() != 0)
        return fail("grid still filled after Close");
    if (tools->GetToolState(drive_tool))
        return fail("Drive tool still pressed after Close");
    frame->open_target(image);

    std::string opened = frame->options().target;
    frame->Close(true);
    wxYield();

    reflash::Options again;
    auto *restored = new reflash::MainFrame(again);
    restored->Show(true);
    wxYield();
    if (restored->options().target != opened)
        return fail("restart did not reopen the last file: " + restored->options().target);
    wxString again_status = restored->GetStatusBar()->GetStatusText(0);
    if (again_status.Contains("Starting"))
        return fail("restored window auto-started");
    if (!again_status.Contains("Target:"))
        return fail(std::string("restored status: ") + again_status.ToStdString());
    auto *tools2 = restored->GetToolBar();
    int start2 = restored->GetMenuBar()->FindMenuItem("&Edit", "&Start\tCtrl+R");
    if (!tools2 || start2 == wxNOT_FOUND || !tools2->GetToolEnabled(start2))
        return fail("Start is not enabled on the restored window");
    restored->Close(true);
    wxYield();
    fprintf(stderr, "gui_e2e: ok (%s)\n", image.c_str());
    return true;
}

} /* namespace */

class E2EApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit())
            return false;
        CallAfter([] {
            g_code = scenario() ? 0 : 1;
            wxTheApp->ExitMainLoop();
        });
        return true;
    }

    int OnRun() override {
        int rc = wxApp::OnRun();
        return g_code != 0 ? g_code : rc;
    }
};

wxIMPLEMENT_APP_NO_MAIN(E2EApp);

int main(int argc, char **argv) {
    const char *display = std::getenv("DISPLAY");
    if (!display || !display[0]) {
        fprintf(stderr, "gui_e2e: no DISPLAY, skip\n");
        return 0;
    }
    return wxEntry(argc, argv);
}
