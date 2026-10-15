#pragma once

// Virtual monitors on Windows, the counterpart of CGVirtualDisplay on macOS, through an IddCx
// driver controlled over its named pipe:
//   - Virtual Display Driver (github.com/VirtualDrivers/Virtual-Display-Driver: MIT, signed):
//     monitors "VDD by MTT", count only, resolutions from its vdd_settings.xml.
//   - virtual-display-rs (github.com/MolotovCherry/virtual-display-rs: MIT): monitors
//     "VirtuDisplay+", any resolution; Nimbo adds its own and leaves the user's alone.
// Display arrangement uses ChangeDisplaySettingsEx batches, and the layout before Nimbo is
// backed up to a file so a crash can be undone with `nimbo_win --restore`.

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "displays.h"
#include "rayneo/core/render_math.h"

namespace rayneo::win {

// Monitor of a virtual display driver (EDID name "VDD by MTT" or "VirtuDisplay+").
bool is_virtual_panel(const OutputInfo& o);

// A driver that creates virtual monitors.
class VirtualDisplayBackend {
public:
    virtual ~VirtualDisplayBackend() = default;
    virtual const char* name() const = 0;
    // Nimbo's monitors: `count` of them at width x height (VDD: its configured modes only).
    virtual bool set_monitors(int count, int width, int height, std::string* error) = 0;
    // Removes Nimbo's monitors (VDD keeps one, which DisplayArrangement::restore detaches).
    virtual void remove_monitors() = 0;
    // What the driver says it has, for the log when monitors do not show up.
    virtual std::string state() const { return ""; }
};

// The first driver that answers: Virtual Display Driver, then virtual-display-rs. nullptr if none.
std::unique_ptr<VirtualDisplayBackend> find_virtual_display_backend();
// A driver is installed, answering or not (for the message when none answers).
bool virtual_display_driver_installed();

class VirtualDisplayDriver {
public:
    // The driver answers on its control pipe.
    static bool available();
    // Its installer left its traces (registry key or C:\VirtualDisplayDriver), answering or not.
    static bool installed();
    // Number of virtual monitors (the driver keeps at least one). Reloads the adapter: the
    // monitors disappear and come back.
    static bool set_count(int count, std::string* error);
    // Monitors the driver has now, on the desktop or not (no pipe needed).
    static int live_monitors();
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
    // rarely wanders into it. With `first` (the computer's own screen as panel 1) it takes
    // cells[0] where it is and the virtual panels the other cells around it (`cells` then has
    // one more entry than `panels`). Applies the change.
    bool arrange(const std::vector<const OutputInfo*>& panels, const std::vector<core::GridCell>& cells,
                 const OutputInfo* glasses, const std::vector<OutputInfo>& outputs, int panel_width, int panel_height,
                 std::string* error, const OutputInfo* first = nullptr);

private:
    bool arrange_after(const std::vector<const OutputInfo*>& panels, const std::vector<core::GridCell>& cells,
                       const OutputInfo* glasses, const OutputInfo& first, int panel_width, int panel_height,
                       std::string* error);
    std::vector<SavedDisplay> saved_;
};

// Virtual monitors the driver created but Windows left off the desktop (after a topology
// change it does): attached to the right of everything at width x height. Returns how many.
int attach_detached_virtual(int width, int height, int limit = 8);

// Every display device Windows knows, attached to the desktop or not, with its monitors: one
// line each, for the log when monitors do not show up.
std::vector<std::string> describe_display_devices();

// Per-monitor scaling (Settings > Display > Scale) of a display, in percent. Uses the same
// DisplayConfig call as the Settings app; Windows remembers it per monitor.
bool set_display_scale(const std::wstring& gdi_name, int percent, std::string* error);

}  // namespace rayneo::win
