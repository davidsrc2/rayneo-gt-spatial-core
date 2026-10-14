#pragma once

// Moves the visible top-level windows of one display into another, keeping their relative
// position (WindowGatherer.swift on macOS; Windows needs no special permission).

#include <windows.h>

namespace rayneo::win {

// Windows whose centre is inside `from` move into `into`; `skip` (the render window) stays.
// Returns how many were moved.
int gather_windows(const RECT& from, const RECT& into, HWND skip);

}  // namespace rayneo::win
