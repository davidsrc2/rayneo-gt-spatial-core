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
#include <newdev.h>  // after setupapi.h
#include <wincrypt.h>

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
  --setup-displays     (administrator) prepare the Virtual Display Driver once: installed from the
                       "driver" folder next to this program if missing, a pool of 6 monitors,
                       duplicate devices removed, the driver restarted, switched on only with Nimbo
  --uninstall-driver   (administrator, the uninstaller) undo --setup-displays
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

// --- the driver shipped with Nimbo ------------------------------------------------------------
// The installer puts the Virtual Display Driver (MIT, signed by the SignPath Foundation) in a
// "driver" folder next to this program. What VDD Control did by hand: its settings folder, its
// publisher trusted (or Windows asks to confirm the install), the package in the driver store
// and a root device for it. What Nimbo did is kept in HKLM\SOFTWARE\Nimbo, so the uninstaller
// removes only that and leaves a driver the user had installed alone.

constexpr const wchar_t* kVddHardwareId = L"Root\\MttVDD";
constexpr const wchar_t* kVddDir = L"C:\\VirtualDisplayDriver";
constexpr const wchar_t* kNimboKey = L"SOFTWARE\\Nimbo";

std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/"));
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void set_state(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kNimboKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::wstring get_state(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kNimboKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// The certificate that signed the driver's catalog.
PCCERT_CONTEXT catalog_signer(const std::wstring& cat) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, cat.c_str(), CERT_QUERY_CONTENT_FLAG_ALL, CERT_QUERY_FORMAT_FLAG_ALL, 0,
                          nullptr, nullptr, nullptr, &store, &msg, nullptr)) {
        return nullptr;
    }
    PCCERT_CONTEXT cert = nullptr;
    DWORD size = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size) && size > 0) {
        std::vector<BYTE> info(size);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, info.data(), &size)) {
            cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
                                              info.data(), nullptr);
        }
    }
    CryptMsgClose(msg);
    CertCloseStore(store, 0);
    return cert;
}

// The driver's publisher in the machine's Trusted Publishers: installed without a prompt.
// true if it was added now (false: it was there already, or it failed).
bool trust_publisher(const std::wstring& cat, bool remove) {
    PCCERT_CONTEXT cert = catalog_signer(cat);
    if (cert == nullptr) return false;
    bool changed = false;
    if (HCERTSTORE trusted = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPublisher")) {
        if (remove) {
            if (PCCERT_CONTEXT found = CertFindCertificateInStore(trusted, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                                  CERT_FIND_EXISTING, cert, nullptr)) {
                changed = CertDeleteCertificateFromStore(found) != FALSE;  // frees `found`
            }
        } else {
            changed = CertAddCertificateContextToStore(trusted, cert, CERT_STORE_ADD_NEW, nullptr) != FALSE;
        }
        CertCloseStore(trusted, 0);
    }
    CertFreeCertificateContext(cert);
    return changed;
}

// Devices with the driver's hardware id, present or not.
std::vector<std::wstring> vdd_root_devices(bool remove) {
    std::vector<std::wstring> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) {
            continue;
        }
        bool match = false;
        for (const wchar_t* id = ids; *id != 0; id += std::wcslen(id) + 1) match = match || _wcsicmp(id, kVddHardwareId) == 0;
        if (!match) continue;
        wchar_t instance[512] = {};
        SetupDiGetDeviceInstanceIdW(set, &d, instance, 512, nullptr);
        out.push_back(instance);
        if (remove) {
            BOOL reboot = FALSE;
            DiUninstallDevice(nullptr, set, &d, 0, &reboot);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// Installs the driver from `dir` (MttVDD.inf, .cat, .dll, vdd_settings.xml) with a pool of 6
// monitors from the start. 0 ok, else an exit code for the app (8).
int install_vdd(const std::wstring& dir) {
    const std::wstring inf = dir + L"\\MttVDD.inf";
    std::printf("setup: installing the Virtual Display Driver shipped with Nimbo\n");
    // Its settings, with the pool already in them: it starts with 6 monitors.
    const std::wstring settings = std::wstring(kVddDir) + L"\\vdd_settings.xml";
    if (!exists(settings)) {
        CreateDirectoryW(kVddDir, nullptr);
        std::ifstream in(dir + L"\\vdd_settings.xml", std::ios::binary);
        const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream(settings, std::ios::binary) << std::regex_replace(
            xml, std::regex(R"(<count>\s*\d+\s*</count>)"), "<count>6</count>", std::regex_constants::format_first_only);
        set_state(L"VddSettingsCreated", L"1");
    }
    std::wstring cat = dir + L"\\mttvdd.cat";
    if (!exists(cat)) cat = dir + L"\\MttVDD.cat";
    if (trust_publisher(cat, false)) set_state(L"VddTrustAdded", L"1");

    wchar_t oem[MAX_PATH] = {};
    PWSTR oem_name = nullptr;
    if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, oem, MAX_PATH, nullptr, &oem_name)) {
        std::printf("setup: the driver package was not accepted (error %lu)\n", GetLastError());
        return 8;
    }
    set_state(L"VddOemInf", oem_name != nullptr ? oem_name : oem);

    if (vdd_root_devices(false).empty()) {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
        SP_DEVINFO_DATA d{};
        d.cbSize = sizeof(d);
        const wchar_t hwid[] = L"Root\\MttVDD\0";  // REG_MULTI_SZ: ends in two NULs
        const bool created =
            set != INVALID_HANDLE_VALUE &&
            SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DICD_GENERATE_ID, &d) &&
            SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid), sizeof(hwid)) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d);
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        if (!created) {
            std::printf("setup: could not create the driver's device (error %lu)\n", GetLastError());
            return 8;
        }
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kVddHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        std::printf("setup: the driver did not install on its device (error %lu)\n", GetLastError());
        return 8;
    }
    set_state(L"VddInstalled", L"1");
    for (int waited = 0; waited < 10000 && vdd_devices().empty(); waited += 500) Sleep(500);
    std::printf("setup: driver installed%s\n", reboot ? " (Windows asks for a restart)" : "");
    return 0;
}

// --uninstall-driver (the uninstaller, as administrator): the on/off tasks go; a driver Nimbo
// installed is removed with what came with it; one the user had is switched on again with its
// own monitor count back.
int uninstall_driver() {
    if (!elevated()) return 2;
    run_quiet(L"schtasks /Delete /F /TN \"Nimbo\\DisplaysOn\"");
    run_quiet(L"schtasks /Delete /F /TN \"Nimbo\\DisplaysOff\"");
    const std::wstring settings = std::wstring(kVddDir) + L"\\vdd_settings.xml";
    if (get_state(L"VddInstalled") == L"1") {
        vdd_root_devices(true);
        const std::wstring oem = get_state(L"VddOemInf");
        if (!oem.empty()) SetupUninstallOEMInfW(oem.c_str(), SUOI_FORCEDELETE, nullptr);
        if (get_state(L"VddTrustAdded") == L"1") {
            const std::wstring dir = exe_dir() + L"\\driver";
            trust_publisher(exists(dir + L"\\mttvdd.cat") ? dir + L"\\mttvdd.cat" : dir + L"\\MttVDD.cat", true);
        }
        if (get_state(L"VddSettingsCreated") == L"1") {
            DeleteFileW(settings.c_str());
            DeleteFileW((settings + L".nimbo-backup").c_str());
            RemoveDirectoryW(kVddDir);
        }
        std::printf("uninstall: driver removed\n");
    } else {
        const std::wstring backup = settings + L".nimbo-backup";
        if (exists(backup)) MoveFileExW(backup.c_str(), settings.c_str(), MOVEFILE_REPLACE_EXISTING);
        for (const auto& id : vdd_devices()) run_quiet(L"pnputil /enable-device \"" + id + L"\"");
        std::printf("uninstall: your own driver left on, with its settings back\n");
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kNimboKey);
    return 0;
}

// Exit codes the app reads: 0 ok, 2 not administrator, 3 driver not installed, 4 settings file
// not understood, 5 no device, 6 could not restart it, 7 no on/off tasks, 8 the shipped driver
// did not install.
int setup_displays() {
    if (!elevated()) {
        std::printf("setup: run as administrator\n");
        return 2;
    }
    // No driver yet and one shipped next to this program (the installer): install it first.
    const std::wstring shipped = exe_dir() + L"\\driver";
    if (vdd_devices().empty() && exists(shipped + L"\\MttVDD.inf")) {
        if (const int r = install_vdd(shipped); r != 0) return r;
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
        if (std::string(argv[i]) == "--uninstall-driver") return uninstall_driver();
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
