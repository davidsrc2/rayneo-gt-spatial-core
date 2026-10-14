#include "window_gatherer.h"

#include <dwmapi.h>

#include <algorithm>
#include <vector>

namespace rayneo::win {

namespace {

struct Context {
    RECT from;
    HWND skip;
    std::vector<HWND> found;
};

BOOL CALLBACK collect(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<Context*>(lp);
    if (hwnd == c->skip || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // dialogs follow their owner
    BOOL cloaked = FALSE;  // hidden UWP frames, windows on other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    const POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    if (PtInRect(&c->from, centre)) c->found.push_back(hwnd);
    return TRUE;
}

}  // namespace

int gather_windows(const RECT& from, const RECT& into, HWND skip) {
    Context c{from, skip, {}};
    EnumWindows(collect, reinterpret_cast<LPARAM>(&c));
    const int fw = std::max(1L, from.right - from.left), fh = std::max(1L, from.bottom - from.top);
    const int iw = into.right - into.left, ih = into.bottom - into.top;
    int moved = 0;
    for (HWND hwnd : c.found) {
        const bool maximized = IsZoomed(hwnd) != FALSE;
        if (maximized) ShowWindow(hwnd, SW_RESTORE);  // a maximized window only moves restored
        RECT r{};
        GetWindowRect(hwnd, &r);
        const int w = std::min<int>(r.right - r.left, iw);
        const int h = std::min<int>(r.bottom - r.top, ih);
        // Same relative position, clamped inside the target.
        int x = into.left + static_cast<int>(static_cast<long long>(r.left - from.left) * iw / fw);
        int y = into.top + static_cast<int>(static_cast<long long>(r.top - from.top) * ih / fh);
        x = std::clamp<int>(x, into.left, into.right - w);
        y = std::clamp<int>(y, into.top, into.bottom - h);
        if (SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE)) ++moved;
        if (maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    }
    return moved;
}

}  // namespace rayneo::win
