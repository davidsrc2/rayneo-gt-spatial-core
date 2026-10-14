#include "virtual_displays.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "rayneo/core/presets.h"

namespace rayneo::win {

namespace {

constexpr const wchar_t* kPipe = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

std::string code_text(const char* what, long code) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s (%ld)", what, code);
    return buf;
}

HANDLE open_pipe() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE h = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipe, 1000)) break;
    }
    return INVALID_HANDLE_VALUE;
}

std::wstring backup_path() {
    wchar_t* base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Nimbo";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\display_backup.txt";
    }
    CoTaskMemFree(base);
    return path;
}

// Queued in the registry (CDS_NORESET); apply() makes the whole batch effective at once.
LONG place(const std::wstring& name, int x, int y, int w, int h, int hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | (hz > 0 ? DM_DISPLAYFREQUENCY : 0);
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    dm.dmPelsWidth = static_cast<DWORD>(w);
    dm.dmPelsHeight = static_cast<DWORD>(h);
    dm.dmDisplayFrequency = static_cast<DWORD>(hz);
    return ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
}

LONG detach(const std::wstring& name) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;  // zero size: off the desktop
    return ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
}

LONG apply() { return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr); }

// DisplayConfig source of a GDI name, for the scale calls.
bool source_of(const std::wstring& gdi_name, LUID* adapter, UINT32* id) {
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && gdi_name == source.viewGdiDeviceName) {
            *adapter = paths[i].sourceInfo.adapterId;
            *id = paths[i].sourceInfo.id;
            return true;
        }
    }
    return false;
}

// Undocumented DisplayConfig types used by the Settings app: scale steps relative to the
// display's recommended one.
constexpr int kGetDpiScale = -3;
constexpr int kSetDpiScale = -4;
struct DpiScaleGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_rel;
    INT32 cur_rel;
    INT32 max_rel;
};
struct DpiScaleSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 scale_rel;
};
constexpr int kScaleSteps[] = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};

}  // namespace

bool is_virtual_panel(const OutputInfo& o) {
    return std::any_of(o.monitors.begin(), o.monitors.end(),
                       [](const std::wstring& m) { return m.rfind(L"VDD by MTT", 0) == 0; });
}

bool VirtualDisplayDriver::available() {
    HANDLE h = open_pipe();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool VirtualDisplayDriver::installed() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return GetFileAttributesW(L"C:\\VirtualDisplayDriver\\vdd_settings.xml") != INVALID_FILE_ATTRIBUTES;
}

bool VirtualDisplayDriver::set_count(int count, std::string* error) {
    HANDLE h = open_pipe();
    if (h == INVALID_HANDLE_VALUE) {
        if (error != nullptr) *error = "Virtual Display Driver not installed or not running";
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    const std::wstring cmd = L"SETDISPLAYCOUNT " + std::to_wstring(std::max(1, count));
    DWORD written = 0;
    const BOOL ok = WriteFile(h, cmd.c_str(), static_cast<DWORD>(cmd.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    if (!ok && error != nullptr) *error = code_text("pipe write failed", static_cast<long>(GetLastError()));
    return ok != FALSE;
}

bool DisplayArrangement::save(const std::vector<OutputInfo>& outputs, std::string* error) {
    saved_.clear();
    for (const auto& o : outputs) {
        if (is_virtual_panel(o)) continue;
        const auto m = current_mode(o.gdi_name);
        if (!m) continue;
        saved_.push_back({o.gdi_name, static_cast<int>(o.desktop.left), static_cast<int>(o.desktop.top), m->width,
                          m->height, m->hz});
    }
    std::ofstream f(backup_path(), std::ios::trunc);
    if (!f) {
        if (error != nullptr) *error = "cannot write the display backup";
        return false;
    }
    for (const auto& s : saved_) {
        f << narrow(s.gdi_name) << ' ' << s.x << ' ' << s.y << ' ' << s.width << ' ' << s.height << ' ' << s.hz << '\n';
    }
    return true;
}

bool DisplayArrangement::backup_exists() {
    const std::wstring p = backup_path();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DisplayArrangement::load_backup() {
    std::ifstream f(backup_path());
    if (!f) return false;
    saved_.clear();
    std::string name;
    SavedDisplay s;
    while (f >> name >> s.x >> s.y >> s.width >> s.height >> s.hz) {
        s.gdi_name.assign(name.begin(), name.end());  // \\.\DISPLAYn: ASCII
        saved_.push_back(s);
    }
    return !saved_.empty();
}

bool DisplayArrangement::restore(const std::vector<OutputInfo>& outputs, std::string* error) {
    for (const auto& o : outputs) {
        if (is_virtual_panel(o)) detach(o.gdi_name);
    }
    for (const auto& s : saved_) place(s.gdi_name, s.x, s.y, s.width, s.height, s.hz);
    const LONG r = apply();
    if (r != DISP_CHANGE_SUCCESSFUL) {
        if (error != nullptr) *error = code_text("display restore failed", r);
        return false;
    }
    DeleteFileW(backup_path().c_str());
    return true;
}

bool DisplayArrangement::arrange(const std::vector<const OutputInfo*>& panels, const std::vector<core::GridCell>& cells,
                                 const OutputInfo* glasses, const std::vector<OutputInfo>& outputs, int panel_width,
                                 int panel_height, std::string* error) {
    // Anchor: top-right corner of the rightmost physical display (not the glasses).
    const OutputInfo* anchor = nullptr;
    for (const auto& o : outputs) {
        if (is_virtual_panel(o) || (glasses != nullptr && o.gdi_name == glasses->gdi_name)) continue;
        if (anchor == nullptr || o.desktop.right > anchor->desktop.right) anchor = &o;
    }
    // Glasses as the only physical display: it stays the primary at the origin.
    const bool glasses_anchor = anchor == nullptr;
    if (glasses_anchor) anchor = glasses;
    if (anchor == nullptr) {
        if (error != nullptr) *error = "no display to place the panels next to";
        return false;
    }
    const int ox = anchor->desktop.right;
    const int oy = anchor->desktop.top;

    std::vector<core::GridCell> grid = cells;
    if (grid.size() != panels.size()) {
        grid.clear();
        for (size_t i = 0; i < panels.size(); ++i) grid.push_back({static_cast<int>(i), 0});
    }
    grid = core::normalized_cells(grid);

    auto queue_panels = [&](int* corner_x, int* corner_y, int* below_x) {
        int best = -1;
        for (size_t i = 0; i < panels.size(); ++i) {
            const int x = ox + grid[i].col * panel_width;
            const int y = oy + grid[i].row * panel_height;
            place(panels[i]->gdi_name, x, y, panel_width, panel_height, 60);
            const int key = grid[i].row * 1000 + grid[i].col;  // bottom row, then rightmost
            if (key > best) {
                best = key;
                *corner_x = x + panel_width;
                *corner_y = y + panel_height;
                *below_x = x;
            }
        }
    };

    int cx = 0, cy = 0, bx = 0;
    queue_panels(&cx, &cy, &bx);
    const auto gm = glasses != nullptr && !glasses_anchor ? current_mode(glasses->gdi_name) : std::nullopt;
    if (gm) place(glasses->gdi_name, cx, cy, gm->width, gm->height, gm->hz);
    LONG r = apply();
    if (r != DISP_CHANGE_SUCCESSFUL && gm) {
        // Corner-only contact refused: below the last panel instead.
        queue_panels(&cx, &cy, &bx);
        place(glasses->gdi_name, bx, cy, gm->width, gm->height, gm->hz);
        r = apply();
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        if (error != nullptr) *error = code_text("display arrangement failed", r);
        return false;
    }
    return true;
}

bool set_display_scale(const std::wstring& gdi_name, int percent, std::string* error) {
    LUID adapter{};
    UINT32 id = 0;
    if (!source_of(gdi_name, &adapter, &id)) {
        if (error != nullptr) *error = "display not found";
        return false;
    }
    DpiScaleGet get{};
    get.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetDpiScale);
    get.header.size = sizeof(get);
    get.header.adapterId = adapter;
    get.header.id = id;
    if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) {
        if (error != nullptr) *error = "scale query failed";
        return false;
    }
    const int recommended = std::abs(get.min_rel);  // index of the recommended step
    const auto* step = std::find(std::begin(kScaleSteps), std::end(kScaleSteps), percent);
    if (step == std::end(kScaleSteps)) {
        if (error != nullptr) *error = "unsupported scale (100, 125, 150, 175, 200...)";
        return false;
    }
    const int rel = std::clamp(static_cast<int>(step - std::begin(kScaleSteps)) - recommended, get.min_rel, get.max_rel);
    if (rel == get.cur_rel) return true;
    DpiScaleSet set{};
    set.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kSetDpiScale);
    set.header.size = sizeof(set);
    set.header.adapterId = adapter;
    set.header.id = id;
    set.scale_rel = rel;
    if (DisplayConfigSetDeviceInfo(&set.header) != ERROR_SUCCESS) {
        if (error != nullptr) *error = "scale change failed";
        return false;
    }
    return true;
}

}  // namespace rayneo::win
