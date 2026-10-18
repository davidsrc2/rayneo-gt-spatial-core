#include "virtual_displays.h"

#include <shlobj.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

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

// Adapter names of the drivers (EnumDisplayDevices' DeviceString), for monitors off the desktop.
bool is_virtual_adapter(const std::wstring& kind) {
    return kind.find(L"Virtual Display Driver") != std::wstring::npos || kind.find(L"VirtuDisplay") != std::wstring::npos ||
           kind.find(L"Parsec Virtual Display") != std::wstring::npos;
}

bool is_virtual_source(const std::wstring& gdi_name) {
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (gdi_name == dev.DeviceName) return is_virtual_adapter(dev.DeviceString);
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    return false;
}

bool is_virtual_panel(const OutputInfo& o) {
    const bool named = std::any_of(o.monitors.begin(), o.monitors.end(), [](const std::wstring& m) {
        return m.rfind(L"VDD by MTT", 0) == 0 || m.rfind(L"VirtuDisplay", 0) == 0 || m.rfind(L"ParsecVDA", 0) == 0;
    });
    return named || is_virtual_source(o.gdi_name);  // Parsec's monitors by their adapter
}

namespace {

// --- Virtual Display Driver ---------------------------------------------------------------

class VddBackend final : public VirtualDisplayBackend {
public:
    const char* name() const override { return "Virtual Display Driver"; }
    // A count change reloads the adapter, and after a reload this driver's pipe may stop
    // answering until Windows restarts (seen on the test PC). So the count is only ever raised,
    // straight to kPool so that adding panels later needs no reload; unused monitors stay
    // detached, and the count is left as is on exit.
    static constexpr int kPool = 6;
    bool set_monitors(int count, int, int, std::string* error) override {
        const int live = VirtualDisplayDriver::live_monitors();
        // While it answers, fill the pool now (one reload), so later layouts never need one.
        if (live < std::max(count, kPool) && VirtualDisplayDriver::available() &&
            VirtualDisplayDriver::set_count(std::max(count, kPool), error)) {
            return true;
        }
        if (live >= count) {
            if (error != nullptr) error->clear();
            return true;
        }
        if (error != nullptr) {
            *error = "the Virtual Display Driver has fewer monitors than needed and does not answer to add more: "
                     "restart Windows once (afterwards Nimbo keeps " + std::to_string(kPool) + " ready)";
        }
        return false;
    }
    void remove_monitors() override {}
};

// --- Parsec VDD -----------------------------------------------------------------------------
// Parsec's virtual display adapter (Root\Parsec\VDA, installed by Parsec or by apps that ship
// it). Monitors are added and removed one at a time by IOCTL on the adapter's device interface,
// with no driver reload, and live only while their owner keeps pinging (UPDATE at least every
// second): when Nimbo stops or crashes they go by themselves. The IOCTLs are the ones
// documented by github.com/nomi-san/parsec-vdd (BSD-2); this is our own code.

constexpr GUID kParsecAdapter = {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
constexpr DWORD kParsecAdd = 0x0022e004, kParsecRemove = 0x0022a008, kParsecUpdate = 0x0022a00c,
                kParsecVersion = 0x0022e010;
constexpr int kParsecMax = 8;  // the driver's limit

HANDLE open_parsec() {
    HDEVINFO set = SetupDiGetClassDevsW(&kParsecAdapter, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE h = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &kParsecAdapter, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH,
                        nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

// One IOCTL: a 32-byte input, a DWORD back (-1 on failure or after 5 s).
long parsec_ioctl(HANDLE h, DWORD code, const void* data = nullptr, size_t size = 0) {
    uint8_t in[32] = {};
    if (data != nullptr && size > 0) std::memcpy(in, data, std::min<size_t>(size, sizeof(in)));
    DWORD out = 0, got = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return -1;
    DeviceIoControl(h, code, in, sizeof(in), &out, sizeof(out), nullptr, &ov);
    const BOOL ok = GetOverlappedResultEx(h, &ov, &got, 5000, FALSE);
    CloseHandle(ov.hEvent);
    return ok ? static_cast<long>(out) : -1;
}

class ParsecBackend final : public VirtualDisplayBackend {
public:
    explicit ParsecBackend(HANDLE h) : h_(h) {
        version_ = parsec_ioctl(h_, kParsecVersion);
        keepalive_ = std::thread([this] {
            while (!stop_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    parsec_ioctl(h_, kParsecUpdate);
                }
                Sleep(100);
            }
        });
    }
    ~ParsecBackend() override {
        remove_monitors();
        stop_ = true;
        if (keepalive_.joinable()) keepalive_.join();
        CloseHandle(h_);
    }
    const char* name() const override { return "Parsec VDD"; }

    // Added or removed to make `count`; the size is set afterwards by the arrangement (the
    // driver offers the usual modes, 1920x1080 among them).
    bool set_monitors(int count, int, int, std::string* error) override {
        count = std::clamp(count, 0, kParsecMax);
        std::lock_guard<std::mutex> lock(mutex_);
        while (static_cast<int>(ours_.size()) > count) {
            remove_locked(ours_.back());
            ours_.pop_back();
        }
        while (static_cast<int>(ours_.size()) < count) {
            const long index = parsec_ioctl(h_, kParsecAdd);
            parsec_ioctl(h_, kParsecUpdate);
            if (index < 0) {
                if (error != nullptr) *error = "Parsec VDD did not add a display (" + std::to_string(ours_.size()) + " added)";
                return false;
            }
            ours_.push_back(static_cast<int>(index));
        }
        return true;
    }

    void remove_monitors() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const int index : ours_) remove_locked(index);
        ours_.clear();
    }

    std::string state() const override {
        return "Parsec VDD version " + std::to_string(version_) + ", " + std::to_string(ours_.size()) + " display(s) added";
    }

private:
    void remove_locked(int index) {
        const uint16_t be = static_cast<uint16_t>(((index & 0xFF) << 8) | ((index >> 8) & 0xFF));  // big-endian index
        parsec_ioctl(h_, kParsecRemove, &be, sizeof(be));
        parsec_ioctl(h_, kParsecUpdate);
    }

    HANDLE h_;
    long version_ = -1;
    std::mutex mutex_;
    std::vector<int> ours_;
    std::atomic<bool> stop_{false};
    std::thread keepalive_;
};

// --- virtual-display-rs ---------------------------------------------------------------------
// JSON messages ended by EOT (0x04): {"Notify":[monitor...]} sets the whole monitor list,
// "State" is answered with {"State":[monitor...]}; monitor = {"id","name","enabled","modes":
// [{"width","height","refresh_rates":[...]}]}.

constexpr const wchar_t* kVdrsPipe = L"\\\\.\\pipe\\virtualdisplaydriver";
constexpr unsigned kFirstNimboId = 41000;  // Nimbo's monitors: 41000 + i; anything else is the user's
constexpr char kEot = '\x04';

HANDLE open_vdrs() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE h = CreateFileW(kVdrsPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kVdrsPipe, 1000)) break;
    }
    return INVALID_HANDLE_VALUE;
}

bool vdrs_send(HANDLE h, const std::string& json) {
    const std::string msg = json + kEot;
    DWORD written = 0;
    return WriteFile(h, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr) && written == msg.size();
}

// The driver's reply to "State", or empty after 2 s.
std::string vdrs_state(HANDLE h) {
    if (!vdrs_send(h, "\"State\"")) return {};
    std::string reply;
    for (int waited = 0; waited < 2000; waited += 10) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return {};
        if (avail == 0) {
            Sleep(10);
            continue;
        }
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), avail, &got, nullptr)) return {};
        reply.append(chunk.data(), got);
        const size_t end = reply.find(kEot);
        if (end != std::string::npos) return reply.substr(0, end);
    }
    return {};
}

// Top-level objects of the first JSON array in `json`, as raw text.
std::vector<std::string> array_objects(const std::string& json) {
    std::vector<std::string> out;
    const size_t open = json.find('[');
    if (open == std::string::npos) return out;
    int depth = 0;
    bool in_string = false;
    size_t start = 0;
    for (size_t i = open; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '[' || c == '{') {
            if (++depth == 2 && c == '{') start = i;
        } else if (c == ']' || c == '}') {
            if (depth == 2 && c == '}') out.push_back(json.substr(start, i - start + 1));
            if (--depth == 0) break;
        }
    }
    return out;
}

long object_id(const std::string& obj) {
    const size_t k = obj.find("\"id\"");
    if (k == std::string::npos) return -1;
    const size_t colon = obj.find(':', k);
    return colon == std::string::npos ? -1 : std::strtol(obj.c_str() + colon + 1, nullptr, 10);
}

class VdrsBackend final : public VirtualDisplayBackend {
public:
    const char* name() const override { return "virtual-display-rs"; }

    bool set_monitors(int count, int width, int height, std::string* error) override {
        std::string ours;
        for (int i = 0; i < count; ++i) {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"id\":%u,\"name\":\"Nimbo %d\",\"enabled\":true,\"modes\":[{\"width\":%d,"
                          "\"height\":%d,\"refresh_rates\":[60]}]}",
                          i > 0 ? "," : "", kFirstNimboId + static_cast<unsigned>(i), i + 1, width, height);
            ours += buf;
        }
        return notify(ours, error);
    }

    void remove_monitors() override { notify("", nullptr); }

    std::string state() const override {
        HANDLE h = open_vdrs();
        if (h == INVALID_HANDLE_VALUE) return "pipe not answering";
        std::string s = vdrs_state(h);
        CloseHandle(h);
        return s.empty() ? "(empty state)" : s;
    }

private:
    // Replaces Nimbo's monitors with `ours` (JSON objects, comma separated), keeping the user's.
    static bool notify(const std::string& ours, std::string* error) {
        HANDLE h = open_vdrs();
        if (h == INVALID_HANDLE_VALUE) {
            if (error != nullptr) *error = "virtual-display-rs is not running";
            return false;
        }
        std::string list;
        for (const auto& obj : array_objects(vdrs_state(h))) {
            const long id = object_id(obj);
            if (id >= static_cast<long>(kFirstNimboId) && id < static_cast<long>(kFirstNimboId) + 16) continue;
            list += (list.empty() ? "" : ",") + obj;
        }
        if (!ours.empty()) list += (list.empty() ? "" : ",") + ours;
        const bool ok = vdrs_send(h, "{\"Notify\":[" + list + "]}");
        CloseHandle(h);
        if (!ok && error != nullptr) *error = code_text("virtual-display-rs pipe write failed", static_cast<long>(GetLastError()));
        return ok;
    }
};

bool vdrs_available() {
    HANDLE h = open_vdrs();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

}  // namespace

namespace {

bool run_task(const wchar_t* name) {
    std::wstring cmd = std::wstring(L"schtasks /Run /TN \"") + name + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

}  // namespace

bool virtual_driver_on(int wait_ms) {
    if (VirtualDisplayDriver::live_monitors() > 0) return true;
    if (!run_task(L"Nimbo\\DisplaysOn")) return false;
    for (int waited = 0; waited < wait_ms; waited += 250) {
        Sleep(250);
        if (VirtualDisplayDriver::live_monitors() > 0) {
            Sleep(1500);  // the rest of the pool comes up with it
            return true;
        }
    }
    return VirtualDisplayDriver::live_monitors() > 0;
}

bool virtual_driver_off() { return run_task(L"Nimbo\\DisplaysOff"); }

bool parsec_vdd_available() {
    HANDLE h = open_parsec();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::unique_ptr<VirtualDisplayBackend> find_virtual_display_backend(bool wait_for_pipe) {
    // Parsec VDD first: no reload, no administrator, its displays go away with us.
    if (HANDLE h = open_parsec(); h != INVALID_HANDLE_VALUE) return std::make_unique<ParsecBackend>(h);
    if (VirtualDisplayDriver::available()) return std::make_unique<VddBackend>();
    // After a reload (Nimbo's last exit, the device switched back on) its pipe takes a while
    // to come back: wait up to 12 s before falling back to another driver or none.
    if (VirtualDisplayDriver::installed()) {
        // Its monitors are there already: no pipe needed unless the count must change.
        if (VirtualDisplayDriver::live_monitors() > 0) return std::make_unique<VddBackend>();
        for (int waited = 0; wait_for_pipe && waited < 12000; waited += 500) {
            Sleep(500);
            if (VirtualDisplayDriver::available()) return std::make_unique<VddBackend>();
        }
    }
    if (vdrs_available()) return std::make_unique<VdrsBackend>();
    return nullptr;
}

bool virtual_display_driver_installed() { return VirtualDisplayDriver::installed(); }

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

int VirtualDisplayDriver::live_monitors() {
    int n = 0;
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (std::wstring(dev.DeviceString).find(L"Virtual Display Driver") != std::wstring::npos) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (EnumDisplayDevicesW(dev.DeviceName, 0, &mon, 0)) ++n;
        }
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    return n;
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
                                 int panel_height, std::string* error, const OutputInfo* first) {
    if (first != nullptr) return arrange_after(panels, cells, glasses, *first, panel_width, panel_height, error);
    // Anchor: top-right corner of the rightmost physical display (not the glasses).
    const OutputInfo* anchor = nullptr;
    for (const auto& o : outputs) {
        if (is_virtual_panel(o) || (glasses != nullptr && o.gdi_name == glasses->gdi_name)) continue;
        if (std::any_of(panels.begin(), panels.end(), [&](const OutputInfo* p) { return p->gdi_name == o.gdi_name; })) {
            continue;
        }
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

bool DisplayArrangement::arrange_after(const std::vector<const OutputInfo*>& panels,
                                       const std::vector<core::GridCell>& cells, const OutputInfo* glasses,
                                       const OutputInfo& first, int panel_width, int panel_height,
                                       std::string* error) {
    std::vector<core::GridCell> grid = cells;
    if (grid.size() != panels.size() + 1) {
        grid.clear();
        for (size_t i = 0; i <= panels.size(); ++i) grid.push_back({static_cast<int>(i), 0});
    }
    const int fw = first.width(), fh = first.height();
    // Cells past the first one start at its far edge; cells before it, one panel each.
    const auto coord = [](int d, int first_size, int size) {
        return d == 0 ? 0 : (d > 0 ? first_size + (d - 1) * size : d * size);
    };
    const core::GridCell c0 = grid[0];
    RECT corner = first.desktop;  // bottom row, then rightmost: where the glasses tuck in
    int best = 0;
    for (size_t i = 0; i < panels.size(); ++i) {
        const core::GridCell c = grid[i + 1];
        const int x = first.desktop.left + coord(c.col - c0.col, fw, panel_width);
        const int y = first.desktop.top + coord(c.row - c0.row, fh, panel_height);
        place(panels[i]->gdi_name, x, y, panel_width, panel_height, 60);
        const int key = (c.row - c0.row) * 1000 + (c.col - c0.col);
        if (key > best) {
            best = key;
            corner = RECT{x, y, x + panel_width, y + panel_height};
        }
    }
    const auto gm = glasses != nullptr ? current_mode(glasses->gdi_name) : std::nullopt;
    if (gm) place(glasses->gdi_name, corner.right, corner.bottom, gm->width, gm->height, gm->hz);
    LONG r = apply();
    if (r != DISP_CHANGE_SUCCESSFUL && gm) {  // corner-only contact refused: below instead
        for (size_t i = 0; i < panels.size(); ++i) {
            const core::GridCell c = grid[i + 1];
            place(panels[i]->gdi_name, first.desktop.left + coord(c.col - c0.col, fw, panel_width),
                  first.desktop.top + coord(c.row - c0.row, fh, panel_height), panel_width, panel_height, 60);
        }
        place(glasses->gdi_name, corner.left, corner.bottom, gm->width, gm->height, gm->hz);
        r = apply();
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        if (error != nullptr) *error = code_text("display arrangement failed", r);
        return false;
    }
    return true;
}

int attach_detached_virtual(int width, int height, int limit) {
    int right = 0, count = 0;
    std::vector<std::wstring> loose;
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        const std::wstring name = dev.DeviceName, kind = dev.DeviceString;
        if (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
                right = std::max(right, static_cast<int>(dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth)));
            }
        } else if (is_virtual_adapter(kind)) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (EnumDisplayDevicesW(name.c_str(), 0, &mon, 0)) loose.push_back(name);  // has a monitor
        }
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    for (const auto& name : loose) {
        if (count >= limit) break;
        if (place(name, right, 0, width, height, 60) == DISP_CHANGE_SUCCESSFUL ||
            place(name, right, 0, width, height, 0) == DISP_CHANGE_SUCCESSFUL) {
            right += width;
            ++count;
        }
    }
    if (count > 0) apply();
    return count;
}

std::vector<std::wstring> spare_virtual_monitors(const std::vector<std::wstring>& exclude) {
    std::vector<std::wstring> out;
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        const std::wstring name = dev.DeviceName;
        if (is_virtual_adapter(dev.DeviceString) && std::find(exclude.begin(), exclude.end(), name) == exclude.end()) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (EnumDisplayDevicesW(name.c_str(), 0, &mon, 0)) out.push_back(name);
        }
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    return out;
}

bool attach_display(const std::wstring& gdi_name, int x, int y, int width, int height) {
    if (place(gdi_name, x, y, width, height, 60) != DISP_CHANGE_SUCCESSFUL &&
        place(gdi_name, x, y, width, height, 0) != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    return apply() == DISP_CHANGE_SUCCESSFUL;
}

int detach_unused_virtual(const std::vector<OutputInfo>& outputs, const std::vector<std::wstring>& keep) {
    int n = 0;
    for (const auto& o : outputs) {
        if (!is_virtual_panel(o) || std::find(keep.begin(), keep.end(), o.gdi_name) != keep.end()) continue;
        if (detach(o.gdi_name) == DISP_CHANGE_SUCCESSFUL) ++n;
    }
    if (n > 0) apply();
    return n;
}

std::vector<std::string> describe_display_devices() {
    std::vector<std::string> out;
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        std::string line = narrow(dev.DeviceName) + " '" + narrow(dev.DeviceString) + "'";
        if (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) line += " [on desktop]";
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) line += " [primary]";
        if (dev.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) line += " [mirroring]";
        DISPLAY_DEVICEW mon{};
        mon.cb = sizeof(mon);
        for (DWORD j = 0; EnumDisplayDevicesW(dev.DeviceName, j, &mon, 0); ++j) {
            line += " | monitor '" + narrow(mon.DeviceString) + "'";
            if (mon.StateFlags & DISPLAY_DEVICE_ACTIVE) line += " active";
            mon = DISPLAY_DEVICEW{};
            mon.cb = sizeof(mon);
        }
        out.push_back(line);
        dev = DISPLAY_DEVICEW{};
        dev.cb = sizeof(dev);
    }
    return out;
}

std::optional<int> get_display_scale(const std::wstring& gdi_name) {
    LUID adapter{};
    UINT32 id = 0;
    if (!source_of(gdi_name, &adapter, &id)) return std::nullopt;
    DpiScaleGet get{};
    get.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetDpiScale);
    get.header.size = sizeof(get);
    get.header.adapterId = adapter;
    get.header.id = id;
    if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) return std::nullopt;
    const int index = std::abs(get.min_rel) + get.cur_rel;
    if (index < 0 || index >= static_cast<int>(std::size(kScaleSteps))) return std::nullopt;
    return kScaleSteps[index];
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
