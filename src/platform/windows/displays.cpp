#include "displays.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <map>
#include <utility>

#include <shobjidl.h>  // IDesktopWallpaper

using Microsoft::WRL::ComPtr;

namespace rayneo::win {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// The RayNeo GT enumerates as "SmartGlasses". Our own virtual monitors (W3) are never the glasses.
bool is_glasses_name(const std::wstring& name) {
    const std::wstring n = lower(name);
    if (n.rfind(L"nimbo", 0) == 0) return false;
    return n.find(L"smartglasses") != std::wstring::npos || n.find(L"rayneo") != std::wstring::npos;
}

// GDI source name (\\.\DISPLAYn) -> friendly names of the monitors it drives.
std::map<std::wstring, std::vector<std::wstring>> monitor_names() {
    std::map<std::wstring, std::vector<std::wstring>> out;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return out;
    }
    paths.resize(path_count);
    for (const auto& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = p.targetInfo.adapterId;
        target.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
        out[source.viewGdiDeviceName].push_back(target.monitorFriendlyDeviceName);
    }
    return out;
}

}  // namespace

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<OutputInfo> enumerate_outputs() {
    std::vector<OutputInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    const auto names = monitor_names();

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            OutputInfo info;
            info.gdi_name = od.DeviceName;
            info.desktop = od.DesktopCoordinates;
            info.adapter_name = ad.Description;
            info.adapter = adapter;
            output.As(&info.output);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(od.Monitor, &mi)) info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            if (const auto it = names.find(info.gdi_name); it != names.end()) info.monitors = it->second;
            info.glasses = std::any_of(info.monitors.begin(), info.monitors.end(), is_glasses_name);
            out.push_back(std::move(info));
        }
    }
    return out;
}

const OutputInfo* find_glasses(const std::vector<OutputInfo>& outputs) {
    for (const auto& o : outputs) {
        if (o.glasses) return &o;
    }
    return nullptr;
}

std::optional<DisplayMode> current_mode(const std::wstring& gdi_name) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdi_name.empty() ? nullptr : gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return std::nullopt;
    return DisplayMode{static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight),
                       static_cast<int>(dm.dmDisplayFrequency)};
}

std::optional<DisplayMode> set_native_mode(const std::wstring& gdi_name, int hz) {
    std::vector<DEVMODEW> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsExW(gdi_name.c_str(), i, &dm, 0); ++i) {
        if (dm.dmBitsPerPel == 32) modes.push_back(dm);
        dm = DEVMODEW{};
        dm.dmSize = sizeof(dm);
    }
    if (modes.empty()) return std::nullopt;
    const auto pixels = [](const DEVMODEW& m) { return static_cast<uint64_t>(m.dmPelsWidth) * m.dmPelsHeight; };
    const uint64_t best = pixels(*std::max_element(modes.begin(), modes.end(), [&](const DEVMODEW& a, const DEVMODEW& b) {
        return pixels(a) < pixels(b);
    }));
    const DEVMODEW* pick = nullptr;
    for (const auto& m : modes) {
        if (pixels(m) != best) continue;
        if (static_cast<int>(m.dmDisplayFrequency) == hz) {
            pick = &m;
            break;
        }
        if (pick == nullptr || m.dmDisplayFrequency > pick->dmDisplayFrequency) pick = &m;
    }
    DEVMODEW target = *pick;
    target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    if (const auto cur = current_mode(gdi_name);
        cur && cur->width == static_cast<int>(target.dmPelsWidth) && cur->height == static_cast<int>(target.dmPelsHeight) &&
        cur->hz == static_cast<int>(target.dmDisplayFrequency)) {
        return cur;
    }
    if (ChangeDisplaySettingsExW(gdi_name.c_str(), &target, nullptr, CDS_FULLSCREEN, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        return std::nullopt;
    }
    return DisplayMode{static_cast<int>(target.dmPelsWidth), static_cast<int>(target.dmPelsHeight),
                       static_cast<int>(target.dmDisplayFrequency)};
}

void restore_mode(const std::wstring& gdi_name) {
    ChangeDisplaySettingsExW(gdi_name.c_str(), nullptr, nullptr, 0, nullptr);
}

int copy_wallpaper(const RECT& from, const std::vector<RECT>& to) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // already MTA: fine too
    int done = 0;
    {
        ComPtr<IDesktopWallpaper> wp;
        if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp)))) {
            std::vector<std::pair<std::wstring, RECT>> monitors;
            UINT count = 0;
            if (SUCCEEDED(wp->GetMonitorDevicePathCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    LPWSTR id = nullptr;
                    if (FAILED(wp->GetMonitorDevicePathAt(i, &id)) || id == nullptr) continue;
                    RECT r{};
                    if (SUCCEEDED(wp->GetMonitorRECT(id, &r))) monitors.emplace_back(id, r);
                    CoTaskMemFree(id);
                }
            }
            const auto at = [](const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top; };
            std::wstring image;
            for (const auto& m : monitors) {
                LPWSTR path = nullptr;
                if (at(m.second, from) && SUCCEEDED(wp->GetWallpaper(m.first.c_str(), &path)) && path != nullptr) {
                    image = path;
                }
                if (path != nullptr) CoTaskMemFree(path);
            }
            if (image.empty()) {  // the same picture everywhere: asked without a monitor
                LPWSTR path = nullptr;
                if (SUCCEEDED(wp->GetWallpaper(nullptr, &path)) && path != nullptr) image = path;
                if (path != nullptr) CoTaskMemFree(path);
            }
            for (const auto& m : monitors) {
                for (const auto& r : to) {
                    if (!image.empty() && at(m.second, r) && SUCCEEDED(wp->SetWallpaper(m.first.c_str(), image.c_str()))) {
                        ++done;
                    }
                }
            }
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return done;
}

std::optional<DISPLAYCONFIG_TOPOLOGY_ID> current_topology() {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) != ERROR_SUCCESS) return std::nullopt;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    DISPLAYCONFIG_TOPOLOGY_ID id{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &id) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return id;
}

bool set_topology(DISPLAYCONFIG_TOPOLOGY_ID topology) {
    UINT32 flag = 0;
    switch (topology) {
        case DISPLAYCONFIG_TOPOLOGY_INTERNAL: flag = SDC_TOPOLOGY_INTERNAL; break;
        case DISPLAYCONFIG_TOPOLOGY_CLONE: flag = SDC_TOPOLOGY_CLONE; break;
        case DISPLAYCONFIG_TOPOLOGY_EXTEND: flag = SDC_TOPOLOGY_EXTEND; break;
        case DISPLAYCONFIG_TOPOLOGY_EXTERNAL: flag = SDC_TOPOLOGY_EXTERNAL; break;
        default: return false;
    }
    return SetDisplayConfig(0, nullptr, 0, nullptr, flag | SDC_APPLY) == ERROR_SUCCESS;
}

}  // namespace rayneo::win
