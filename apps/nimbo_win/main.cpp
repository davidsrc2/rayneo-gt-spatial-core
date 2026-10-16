// nimbo_win: command-line front end of nimbo.dll (rayneo/platform/nimbo_win.h), the Windows
// spatial server. Virtual monitors (or an existing display) fixed in space on the RayNeo GT.
//
//   nimbo_win                     glasses + 2 virtual monitors (Virtual Display Driver)
//   nimbo_win --displays 0        an existing display as one panel
//   nimbo_win --windowed --sim    no glasses: a window on the main screen, simulated head
//   nimbo_win --help

#include <windows.h>

#include <devguid.h>
#include <setupapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rayneo/platform/nimbo_win.h"

namespace {

const char* kUsage = R"(usage: nimbo_win [options]
  --app                start from the app's own settings (%LOCALAPPDATA%\Nimbo\settings.json and
                       calibration.json), as Nimbo.exe would; options after it change them
space at real size (the app's mode)
  --space              glasses put in follow, every panel as large as their image, fixed in the
                       room on both axes (back to pinned on exit); mono unless --sbs
  --anchor             with --space: panels frozen while the head holds still
  --tilt K             panels above / below: 1 face you (default) .. 0 upright
  --laptop-screen      the computer's own (primary) screen is panel 1, the rest virtual
AI 3D (cinema mode, Ctrl+Alt+P)
  --depth3d            the cinema panel in depth from a depth map per frame (Ctrl+Alt+3); needs
                       --sbs and the glasses in 3D mode. ONNX Runtime + DirectML, model downloaded once
  --depth-strength F   parallax at the nearest depth, in panel widths (0.015)
  --depth-plane F      depth left on the panel's plane, 0 far .. 1 near (0.35)
layout
  --displays N         virtual monitors around you (2; needs the Virtual Display Driver);
                       0 = show an existing display
  --layout C,R;C,R...  grid cell per monitor (row 0 = top), e.g. "0,0;0,1" = two stacked
  --vd-size WxH        virtual monitor resolution, one the driver offers (1920x1080)
  --vd-scale PCT       Windows scaling of the virtual monitors (150: large, sharp text)
  --fit                all panels visible at once (each at a fraction of full resolution)
  --arc DEG            horizontal size of each panel (38; 50 with --displays 0)
  --gap DEG            space between panels (1)
  --no-gather          do not move windows from the glasses display to panel 1
  --no-gaze-cursor     keep the cursor where it is when you look at another panel
  --dim B              brightness of the panels you are not looking at, 0..1 (1 = off)
  --window TEXT[@Y,P,S] float the window whose title or app contains TEXT as a panel of its own
                       (default: centred above the monitors); repeatable
  --list-windows       print the windows that can float (app and title), then exit
  --setup-displays     (administrator) prepare the Virtual Display Driver once: a pool of 6
                       monitors, duplicate devices removed, the driver restarted; then exit
  --pinned             the glasses are in their pinned mode: panels stay put, fitted inside the image
  --glasses-mode M     put the glasses in follow, steady or pinned (pinned implies --pinned)
  --brightness N       glasses' brightness 1..8 (8 = factory level)
  --track AXES         head rotations that move the panels: h (left/right, default), v (up/down,
                       for stacked monitors) or both
  --curved             curved panels (default: flat monitors angled towards you)
  --curvature K        0 flat .. 1 fully curved around you
  --distance M         panel distance in metres (2.0)
optics
  --mono / --sbs       single view (glasses 2D) / half side-by-side (glasses 3D)
  --ipd MM             interpupillary distance (63)
  --fov DEG            per-eye vertical FOV of the optics (24.5, or the value tuned with
                       Ctrl+Alt+- / Ctrl+Alt+= which is remembered)
  --k1 K --k2 K        barrel pre-distortion (0 = off)
tracking
  --calib FILE         sensor_to_head calibration (default ./calibration.json if present)
  --dead-zone DEG      stabilizer dead zone (1.0): smaller follows sooner, larger is stiller
  --sim                no glasses: simulated head (Ctrl+Alt+arrows to look around)
  --sim-sweep DEG      sim: automatic left/right sweep of +-DEG
display
  --windowed           render in a window on the main screen instead of the glasses
  --source S           --displays 0 only: auto | glasses | primary | <index from the list>
  --60hz               keep the glasses' current refresh rate (default: 120 Hz)
  --duration S         exit after S seconds
  --restore            undo the display changes of a run that crashed, then exit
hotkeys: Ctrl+Alt+C recenter, Ctrl+Alt+Q quit,
         Ctrl+Alt+- / Ctrl+Alt+= field of view (tune until the panel stays put when turning),
         Ctrl+Alt+arrows look around (--sim)
)";

struct Cli {
    nimbo_options options = nimbo_default_options();
    double duration_s = 0;
    bool restore = false;
};

// "A<sep>B" with two integers, nothing else.
bool parse_pair(const char* text, char sep, int* a, int* b) {
    char* end = nullptr;
    const long x = std::strtol(text, &end, 10);
    if (end == text || *end != sep) return false;
    const char* rest = end + 1;
    const long y = std::strtol(rest, &end, 10);
    if (end == rest || *end != '\0') return false;
    *a = static_cast<int>(x);
    *b = static_cast<int>(y);
    return true;
}

bool copy_arg(char* dst, size_t size, const char* src) {
    if (src == nullptr || std::strlen(src) >= size) return false;
    std::snprintf(dst, size, "%s", src);
    return true;
}

bool parse(int argc, char** argv, Cli* cli) {
    nimbo_options& o = cli->options;
    bool optics_chosen = false;  // --mono / --sbs given
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        auto number = [&](float* out) {
            const char* v = value();
            if (v == nullptr) return false;
            char* end = nullptr;
            *out = std::strtof(v, &end);
            return end != v && *end == '\0';
        };
        float f = 0;
        if (a == "--displays" && number(&f) && f >= 0 && f <= NIMBO_MAX_PANELS) o.displays = static_cast<int>(f);
        else if (a == "--layout") {
            const char* v = value();
            if (v == nullptr) return false;
            o.layout_count = 0;
            std::stringstream ss(v);
            std::string cell;
            while (std::getline(ss, cell, ';')) {
                if (o.layout_count == NIMBO_MAX_PANELS) return false;
                if (!parse_pair(cell.c_str(), ',', &o.layout_cols[o.layout_count], &o.layout_rows[o.layout_count])) {
                    return false;
                }
                ++o.layout_count;
            }
            if (o.layout_count == 0) return false;
            o.displays = o.layout_count;
        }
        else if (a == "--vd-size") {
            const char* v = value();
            if (v == nullptr || !parse_pair(v, 'x', &o.vd_width, &o.vd_height) || o.vd_width < 640 || o.vd_height < 480) {
                return false;
            }
        }
        else if (a == "--vd-scale" && number(&f)) o.vd_scale = static_cast<int>(f);
        else if (a == "--fit") o.fit = true;
        else if (a == "--no-gather") o.gather_windows = false;
        else if (a == "--no-gaze-cursor") o.gaze_cursor = false;
        else if (a == "--track") {
            const char* v = value();
            if (v == nullptr) return false;
            const std::string axes = v;
            if (axes == "both") o.tracking_axes = 0;
            else if (axes == "h") o.tracking_axes = 1;
            else if (axes == "v") o.tracking_axes = 2;
            else return false;
        }
        else if (a == "--full-tracking") o.tracking_axes = 0;
        else if (a == "--window") {
            const char* v = value();
            if (v == nullptr || o.floating_count >= NIMBO_MAX_PANELS) return false;
            std::string text = v, pose;
            if (const size_t at = text.find('@'); at != std::string::npos) {
                pose = text.substr(at + 1);
                text = text.substr(0, at);
            }
            // Case-insensitive match on title or executable name.
            auto lower = [](std::string s) {
                for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                return s;
            };
            nimbo_window list[64];
            const int n = nimbo_list_windows(list, 64);
            int found = -1;
            for (int k = 0; k < n && found < 0; ++k) {
                if (lower(list[k].title).find(lower(text)) != std::string::npos ||
                    lower(list[k].app).find(lower(text)) != std::string::npos) {
                    found = k;
                }
            }
            if (found < 0) {
                std::printf("--window: no window matches \"%s\"\n", text.c_str());
                return false;
            }
            const int slot = o.floating_count++;
            o.floating_window[slot] = list[found].hwnd;
            o.floating_scale[slot] = 0;  // centred above the monitors
            if (!pose.empty()) {  // Y,P,S
                float* dst[3] = {&o.floating_yaw[slot], &o.floating_pitch[slot], &o.floating_scale[slot]};
                const char* c = pose.c_str();
                for (int k = 0; k < 3; ++k) {
                    char* end = nullptr;
                    *dst[k] = std::strtof(c, &end);
                    if (end == c || (k < 2 && *end != ',') || (k == 2 && *end != '\0')) return false;
                    c = end + 1;
                }
            }
        }
        else if (a == "--pinned") { o.glasses_pinned = true; o.glasses_mode = 1; }
        else if (a == "--glasses-mode") {
            const char* v = value();
            if (v == nullptr) return false;
            const std::string m = v;
            if (m == "follow") o.glasses_mode = 0;
            else if (m == "pinned") o.glasses_mode = 1;
            else if (m == "steady") o.glasses_mode = 2;
            else return false;
            o.glasses_pinned = o.glasses_mode == 1;
        }
        else if (a == "--brightness" && number(&f) && f >= 1 && f <= 8) o.brightness = static_cast<int>(f);
        else if (a == "--dim" && number(&f)) o.dim_others = f;
        else if (a == "--arc" && number(&f)) o.arc_deg = f;
        else if (a == "--gap" && number(&f)) o.gap_deg = f;
        else if (a == "--curved") o.curvature = 1;
        else if (a == "--curvature" && number(&f) && f >= 0 && f <= 1) o.curvature = f;
        else if (a == "--distance" && number(&f)) o.distance_m = f;
        else if (a == "--mono") { o.side_by_side = 0; optics_chosen = true; }
        else if (a == "--sbs") { o.side_by_side = 1; optics_chosen = true; }
        else if (a == "--ipd" && number(&f)) o.ipd_mm = f;
        else if (a == "--fov" && number(&f) && f > 0) o.fov_y_deg = f;
        else if (a == "--k1" && number(&f)) o.k1 = f;
        else if (a == "--k2" && number(&f)) o.k2 = f;
        else if (a == "--dead-zone" && number(&f)) o.dead_zone_deg = f;
        else if (a == "--calib") {
            if (!copy_arg(o.calibration_path, sizeof(o.calibration_path), value())) return false;
            o.calibration_required = true;
        }
        else if (a == "--sim") o.sim = true;
        else if (a == "--sim-sweep" && number(&f)) { o.sim = true; o.sim_sweep_deg = f; }
        else if (a == "--windowed") o.windowed = true;
        else if (a == "--source") { if (!copy_arg(o.source, sizeof(o.source), value())) return false; }
        else if (a == "--60hz") o.refresh120 = false;
        else if (a == "--duration" && number(&f)) cli->duration_s = f;
        else if (a == "--restore") cli->restore = true;
        else if (a == "--app") {}  // applied before parsing (load_app_settings)
        else if (a == "--space") {
            o.glasses_mode = 0;
            o.glasses_pinned = false;
            o.recenter_glasses = false;
            o.real_size = true;
            if (o.dead_zone_deg >= 0) o.dead_zone_deg = -1.0f;  // world-locked
        }
        else if (a == "--anchor") o.dead_zone_deg = -1.3f;  // world-locked, frozen within 0.3 deg
        else if (a == "--tilt" && number(&f) && f >= 0 && f <= 1) o.tilt = f;
        else if (a == "--laptop-screen") o.laptop_screen = true;
        else if (a == "--depth3d") o.depth3d = true;
        else if (a == "--depth-strength" && number(&f) && f > 0 && f <= 0.05f) o.depth_strength = f;
        else if (a == "--depth-plane" && number(&f) && f >= 0 && f <= 1) o.depth_plane = f;
        else return false;
    }
    // The space at real size is the app's mode, with the glasses in 2D: mono unless --sbs.
    if (o.real_size && !optics_chosen) o.side_by_side = 0;
    return true;
}

// --app: the options the Windows app would start with, from its settings file.
bool load_app_settings(nimbo_options* o) {
    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir)) == 0) return false;
    const std::string base = std::string(dir) + "\\Nimbo\\";
    nimbo_settings s{};
    if (!nimbo_load_settings((base + "settings.json").c_str(), &s)) return false;
    nimbo_options_from_settings(&s, o);
    const std::string calib = base + "calibration.json";
    if (GetFileAttributesA(calib.c_str()) != INVALID_FILE_ATTRIBUTES) {
        copy_arg(o->calibration_path, sizeof(o->calibration_path), calib.c_str());
    }
    std::printf("settings: %ssettings.json\n", base.c_str());
    return true;
}

// --- --setup-displays ----------------------------------------------------------------------
// What had to be done by hand in PowerShell: the Virtual Display Driver's count raised to a
// pool of 6 (Nimbo never changes it afterwards: a reload can leave its pipe silent until
// Windows restarts), duplicate devices of it removed, and the driver restarted.

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

int run_quiet(std::wstring cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::vector<std::wstring> vdd_devices() {
    std::vector<std::wstring> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t desc[256] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(desc),
                                               sizeof(desc), nullptr) ||
            std::wcsstr(desc, L"Virtual Display Driver") == nullptr) {
            continue;
        }
        wchar_t id[512] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &d, id, 512, nullptr)) out.push_back(id);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

int live_vdd_monitors() {
    int n = 0;
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (std::wcsstr(dev.DeviceString, L"Virtual Display Driver") != nullptr) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (EnumDisplayDevicesW(dev.DeviceName, 0, &mon, 0)) ++n;
        }
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    return n;
}

// Exit codes the app reads: 0 ok, 2 not administrator, 3 driver not installed, 4 settings file
// not understood, 5 no device, 6 could not restart it.
int setup_displays() {
    if (!elevated()) {
        std::printf("setup: run as administrator\n");
        return 2;
    }
    const char* path = "C:\\VirtualDisplayDriver\\vdd_settings.xml";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("setup: the Virtual Display Driver is not installed (%s missing)\n", path);
        return 3;
    }
    const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::regex count(R"(<count>\s*\d+\s*</count>)");
    if (!std::regex_search(xml, count)) {
        std::printf("setup: no <count> in %s\n", path);
        return 4;
    }
    const std::string backup = std::string(path) + ".nimbo-backup";
    if (GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES) std::ofstream(backup, std::ios::binary) << xml;
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        << std::regex_replace(xml, count, "<count>6</count>", std::regex_constants::format_first_only);
    std::printf("setup: driver set to 6 monitors\n");

    const auto devices = vdd_devices();
    if (devices.empty()) {
        std::printf("setup: no Virtual Display Driver device found\n");
        return 5;
    }
    for (size_t i = 1; i < devices.size(); ++i) {  // installed twice: one is enough
        run_quiet(L"pnputil /remove-device \"" + devices[i] + L"\"");
        std::printf("setup: removed a duplicate driver device\n");
    }
    if (run_quiet(L"pnputil /restart-device \"" + devices[0] + L"\"") != 0) {
        run_quiet(L"pnputil /disable-device \"" + devices[0] + L"\"");
        Sleep(1500);
        if (run_quiet(L"pnputil /enable-device \"" + devices[0] + L"\"") != 0) {
            std::printf("setup: could not restart the driver (restart Windows once)\n");
            return 6;
        }
    }
    for (int waited = 0; waited < 15000 && live_vdd_monitors() < 6; waited += 500) Sleep(500);
    std::printf("setup: the driver has %d monitors\n", live_vdd_monitors());

    // Tasks that switch the driver on and off as administrator without a prompt, so its
    // monitors only exist while Nimbo runs (Nimbo starts them; they slowed the PC otherwise).
    const std::wstring& id = devices[0];
    const int on = run_quiet(L"schtasks /Create /F /TN \"Nimbo\\DisplaysOn\" /SC ONCE /ST 00:00 /RL HIGHEST /TR "
                             L"\"pnputil /enable-device " + id + L"\"");
    const int off = run_quiet(L"schtasks /Create /F /TN \"Nimbo\\DisplaysOff\" /SC ONCE /ST 00:00 /RL HIGHEST /TR "
                              L"\"pnputil /disable-device " + id + L"\"");
    if (on != 0 || off != 0) {
        std::printf("setup: could not create the on/off tasks (the driver stays on)\n");
        return 7;
    }
    run_quiet(L"pnputil /disable-device \"" + id + L"\"");  // off until Nimbo starts
    std::printf("setup: done; the virtual monitors exist only while Nimbo runs\n");
    return 0;
}

std::atomic<bool> g_quit{false};

BOOL WINAPI console_handler(DWORD type) {
    // Ctrl+C / closing the console: stop cleanly so stream-off is sent and the displays restored.
    g_quit = true;
    // Closing the console ends the process when this returns: give the main thread time.
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) Sleep(5000);
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            std::printf("%s", kUsage);
            return 0;
        }
        if (std::string(argv[i]) == "--setup-displays") return setup_displays();
        if (std::string(argv[i]) == "--list-windows") {
            nimbo_window list[64];
            const int n = nimbo_list_windows(list, 64);
            for (int k = 0; k < n; ++k) std::printf("  %-20s  %s\n", list[k].app, list[k].title);
            if (n == 0) std::printf("no windows\n");
            return 0;
        }
    }
    Cli cli;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--app" && !load_app_settings(&cli.options)) {
            std::printf("--app: could not read the app's settings file\n");
            return 1;
        }
    }
    if (!parse(argc, argv, &cli)) {
        std::printf("%s", kUsage);
        return 1;
    }
    // Physical pixels everywhere (output rectangles, window size, capture).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (cli.restore) {
        if (!nimbo_restore_displays()) std::printf("nothing to restore\n");
        return 0;
    }

    // One server at a time across processes: two would fight over the displays and hotkeys.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\NimboServer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("Nimbo is already running: quit it first (Ctrl+Alt+Q)\n");
        return 1;
    }
    if (nimbo_restore_displays()) std::printf("(the previous run did not exit cleanly: displays restored)\n");
    SetConsoleCtrlHandler(console_handler, TRUE);

    nimbo_server* server = nimbo_create();
    int code = 0;
    if (!nimbo_start(server, &cli.options)) {
        nimbo_status st{};
        nimbo_get_status(server, &st);
        std::printf("start failed: %s\n", st.message);
        code = 1;
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        while (!g_quit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            nimbo_status st{};
            nimbo_get_status(server, &st);
            if (st.state == NIMBO_STOPPED) break;  // stopped on its own: Ctrl+Alt+Q, glasses unplugged
            if (cli.duration_s > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= cli.duration_s) {
                break;
            }
        }
    }
    nimbo_destroy(server);  // stops: restores the displays
    if (single != nullptr) CloseHandle(single);
    return code;
}
