#pragma once

// Virtual monitors on Windows through the Virtual Display Driver (github.com/VirtualDrivers/
// Virtual-Display-Driver: MIT, IddCx, signed), the counterpart of CGVirtualDisplay on macOS.
// The driver is controlled through its named pipe; its monitors announce themselves as
// "VDD by MTT". Display arrangement uses ChangeDisplaySettingsEx batches, and the layout
// before Nimbo is backed up to a file so a crash can be undone with `nimbo_win --restore`.

#include <windows.h>

#include <string>
#include <vector>

#include "displays.h"
#include "rayneo/core/render_math.h"

namespace rayneo::win {

// Monitor of the Virtual Display Driver (EDID name "VDD by MTT").
bool is_virtual_panel(const OutputInfo& o);

class VirtualDisplayDriver {
public:
    // The driver answers on its control pipe.
    static bool available();
    // Number of virtual monitors (the driver keeps at least one). Reloads the adapter: the
    // monitors disappear and come back.
    static bool set_count(int count, std::string* error);
};

// Position and mode of an attached display, for the backup and the restore.
struct SavedDisplay {
    std::wstring gdi_name;
    int x = 0, y = 0, width = 0, height = 0, hz = 0;
};

class DisplayArrangement {
public:
    // Snapshot of every attached physical display, written to the backup file.
    bool save(const std::vector<OutputInfo>& outputs, std::string* error);
    // Physical displays back where they were, virtual panels detached, backup deleted.
    bool restore(const std::vector<OutputInfo>& outputs, std::string* error);
    static bool backup_exists();
    // Loads the backup left by a previous run (crash) into this object.
    bool load_backup();

    // Panels (left -> right in `cells` order) in a grid to the right of the physical displays,
    // the glasses surface tucked diagonally off the bottom-right panel's corner, so the cursor
    // rarely wanders into it. Applies the change.
    bool arrange(const std::vector<const OutputInfo*>& panels, const std::vector<core::GridCell>& cells,
                 const OutputInfo* glasses, const std::vector<OutputInfo>& outputs, int panel_width, int panel_height,
                 std::string* error);

private:
    std::vector<SavedDisplay> saved_;
};

// Per-monitor scaling (Settings > Display > Scale) of a display, in percent. Uses the same
// DisplayConfig call as the Settings app; Windows remembers it per monitor.
bool set_display_scale(const std::wstring& gdi_name, int percent, std::string* error);

}  // namespace rayneo::win
