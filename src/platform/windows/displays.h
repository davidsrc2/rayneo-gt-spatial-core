#pragma once

// Display outputs on Windows: which one is the glasses, desktop rectangles, and the
// temporary native-mode switch (the equivalent of Displays.swift).

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace rayneo::win {

struct OutputInfo {
    std::wstring gdi_name;                  // \\.\DISPLAY3 (EnumDisplaySettings, ChangeDisplaySettingsEx)
    std::vector<std::wstring> monitors;     // friendly names of the monitors on this source (> 1: mirrored)
    RECT desktop{};                         // virtual desktop coordinates, physical pixels
    bool primary = false;
    bool glasses = false;                   // a monitor on this source is the RayNeo GT ("SmartGlasses")
    std::wstring adapter_name;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;

    int width() const { return desktop.right - desktop.left; }
    int height() const { return desktop.bottom - desktop.top; }
    bool mirrored() const { return monitors.size() > 1; }
};

// Every active output of every adapter. The process must be per-monitor DPI aware for the
// rectangles to be in physical pixels.
std::vector<OutputInfo> enumerate_outputs();

const OutputInfo* find_glasses(const std::vector<OutputInfo>& outputs);

struct DisplayMode {
    int width = 0;
    int height = 0;
    int hz = 0;
};

std::optional<DisplayMode> current_mode(const std::wstring& gdi_name);

// Largest mode of the output at `hz` (or its highest rate at that size), applied as a
// temporary mode (CDS_FULLSCREEN): restore_mode() returns to the user's setting.
std::optional<DisplayMode> set_native_mode(const std::wstring& gdi_name, int hz);
void restore_mode(const std::wstring& gdi_name);

std::string narrow(const std::wstring& w);

}  // namespace rayneo::win
