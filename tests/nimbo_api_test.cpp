// C ABI of nimbo.dll on CI (no glasses, no driver): settings file round trip, mapping to
// server options, environment queries and a start that fails cleanly.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "rayneo/platform/nimbo_win.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "nimbo_api_test\\settings.json";
    DeleteFileA(path.c_str());

    nimbo_settings s{};
    check(nimbo_load_settings(path.c_str(), &s), "missing settings file: defaults");
    check(s.cell_count == 2 && s.text_size == 3 && s.gaze_cursor, "defaults match macOS");

    s.cell_count = 3;
    s.cols[0] = 5; s.rows[0] = 1;  // normalised by the mapping
    s.cols[1] = 6; s.rows[1] = 1;
    s.cols[2] = 5; s.rows[2] = 2;
    s.text_size = 0;
    s.stereo = true;
    s.dim_others = false;
    s.size_deg = 42;
    check(nimbo_save_settings(path.c_str(), &s), "save (creates the folder)");
    nimbo_settings back{};
    check(nimbo_load_settings(path.c_str(), &back) && back.cell_count == 3 && back.cols[1] == 6 && back.stereo &&
              !back.dim_others && back.text_size == 0,
          "load what was saved");

    nimbo_options o = nimbo_default_options();
    check(o.displays == 2 && o.side_by_side == -1 && o.hotkeys && std::strcmp(o.source, "auto") == 0, "default options");
    nimbo_options_from_settings(&back, &o);
    check(o.displays == 3 && o.layout_count == 3 && o.layout_cols[0] == 0 && o.layout_rows[0] == 0 &&
              o.layout_cols[2] == 0 && o.layout_rows[2] == 1,
          "layout normalised to the top-left cell");
    // Side by side follows the glasses' 3D mode, whatever the old "stereo" setting says.
    check(o.vd_scale == 100 && o.side_by_side == -1 && o.dim_others == 1.0f && o.arc_deg == 42.0f,
          "text size, side by side auto, dim, size mapped");

    check(!nimbo_glasses_connected(), "no glasses on CI");
    check(!nimbo_virtual_driver_available(), "no virtual display driver on CI");

    nimbo_server* server = nimbo_create();
    o = nimbo_default_options();  // glasses required: start must fail with a reason
    nimbo_status st{};
    check(!nimbo_start(server, &o), "start without glasses fails");
    nimbo_get_status(server, &st);
    std::printf("  reason: %s\n", st.message);
    check(st.state == NIMBO_STOPPED && std::strlen(st.message) > 0, "stopped with a reason");
    nimbo_destroy(server);
    return failures == 0 ? 0 : 1;
}
