#ifndef NIMBO_WIN_H
#define NIMBO_WIN_H

/*
 * C ABI of nimbo.dll, the Windows spatial server (capture, render, virtual monitors, IMU),
 * for the Windows app (C# P/Invoke) and the nimbo_win CLI. Strings are UTF-8. Every call is
 * safe from any thread unless noted; the server runs on a thread of its own.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(NIMBO_BUILD_DLL)
#define NIMBO_API __declspec(dllexport)
#elif defined(_WIN32)
#define NIMBO_API __declspec(dllimport)
#else
#define NIMBO_API
#endif

#define NIMBO_MAX_PANELS 8

/* ---------------------------------------------------------------------------------------
 * Server options. Start from nimbo_default_options().
 * --------------------------------------------------------------------------------------- */

typedef struct {
    /* layout */
    int displays;                       /* virtual monitors (needs the Virtual Display Driver); 0 = an existing display */
    int layout_count;                   /* grid cells below; 0 = one row */
    int layout_cols[NIMBO_MAX_PANELS];
    int layout_rows[NIMBO_MAX_PANELS];  /* 0 = top */
    int vd_width, vd_height;            /* virtual monitor resolution (a mode the driver offers) */
    int vd_scale;                       /* Windows scaling of the virtual monitors, percent */
    float arc_deg;                      /* panel width; <= 0: 38 with virtual monitors, 50 otherwise */
    float gap_deg;
    float distance_m;
    float curvature;                    /* 0 flat .. 1 cylinder */
    bool fit;                           /* all panels visible at once */
    bool gather_windows;                /* windows on the glasses display move to panel 1 */
    bool gaze_cursor;                   /* the cursor follows the panel you look at */
    float dim_others;                   /* brightness of the panels you are not looking at, 1 = off */
    /* optics */
    int side_by_side;                   /* -1 auto (SBS on the glasses, mono windowed), 0 mono, 1 half-SBS */
    float ipd_mm;
    float fov_y_deg;                    /* <= 0: the value tuned with Ctrl+Alt+-/= (saved), else 24.5 */
    float k1, k2;                       /* barrel pre-distortion */
    /* tracking */
    char calibration_path[260];         /* sensor_to_head JSON; missing file = identity */
    bool calibration_required;
    float dead_zone_deg;
    bool sim;                           /* simulated head instead of the IMU */
    float sim_sweep_deg;
    /* display */
    bool windowed;                      /* a window on the main screen instead of the glasses */
    char source[32];                    /* displays == 0: "auto" | "glasses" | "primary" | index */
    bool refresh120;                    /* glasses at 120 Hz */
    bool hotkeys;                       /* Ctrl+Alt+C / Q / - / = (and arrows with sim) */
    int tracking_axes;                  /* head rotations that move the panels: 0 both, 1 left/right, 2 up/down */
    bool glasses_pinned;                /* glasses in their pinned mode: panels stay put, fitted inside the image */
    int pose_count;                     /* hand-placed monitors (layout editor); 0 or != displays: the grid */
    float pose_yaw[NIMBO_MAX_PANELS];   /* degrees right, at 2 m with arc_deg-sized panels */
    float pose_pitch[NIMBO_MAX_PANELS]; /* degrees up */
    float pose_scale[NIMBO_MAX_PANELS]; /* size relative to arc_deg */
    int glasses_mode;                   /* put the glasses in this mode at start: 0 follow, 1 pinned, 2 steady; -1 leave */
    int brightness;                     /* glasses' brightness 1..8 at start; 0 leave */
} nimbo_options;

NIMBO_API nimbo_options nimbo_default_options(void);

/* ---------------------------------------------------------------------------------------
 * Server
 * --------------------------------------------------------------------------------------- */

typedef struct nimbo_server nimbo_server;

typedef enum {
    NIMBO_STOPPED = 0,
    NIMBO_STARTING = 1,
    NIMBO_CALIBRATING = 2, /* IMU bias calibration: glasses still on the table */
    NIMBO_TRACKING = 3,
} nimbo_state;

typedef struct {
    nimbo_state state;
    int panel_count;
    int gaze_panel;        /* panel looked at, -1 between panels or not tracking */
    float fov_y_deg;       /* current, after Ctrl+Alt+-/= */
    float render_fps;      /* last second */
    float capture_fps;
    float latency_ms;      /* IMU sample -> scanout, last second */
    char message[256];     /* why it stopped or failed to start; empty otherwise */
} nimbo_status;

NIMBO_API nimbo_server* nimbo_create(void);
NIMBO_API void nimbo_destroy(nimbo_server* s); /* stops first */

/* Blocking until the server is up (a few seconds: display changes) or failed; false with
 * status.message on failure. Only one server per process can run. */
NIMBO_API bool nimbo_start(nimbo_server* s, const nimbo_options* options);
/* Blocking until the displays are restored. */
NIMBO_API void nimbo_stop(nimbo_server* s);

/* Applies what changes without a restart: distance, arc, gap, curvature, fov, ipd,
 * side_by_side, k1/k2, dim_others, gaze_cursor. Layout, displays and resolution need
 * nimbo_stop + nimbo_start. */
NIMBO_API void nimbo_apply(nimbo_server* s, const nimbo_options* options);
NIMBO_API void nimbo_recenter(nimbo_server* s);
NIMBO_API void nimbo_adjust_fov(nimbo_server* s, float delta_deg);
NIMBO_API void nimbo_get_status(nimbo_server* s, nimbo_status* out);

/* Log lines of every server in the process (and the calibration), from any thread. NULL:
 * stdout. */
typedef void (*nimbo_log_fn)(void* ctx, const char* line);
NIMBO_API void nimbo_set_log(nimbo_log_fn fn, void* ctx);

/* ---------------------------------------------------------------------------------------
 * Environment
 * --------------------------------------------------------------------------------------- */

NIMBO_API bool nimbo_glasses_connected(void);         /* the SmartGlasses display is present */
NIMBO_API bool nimbo_virtual_driver_available(void);  /* the Virtual Display Driver answers */
/* Undoes the display changes of a run that crashed. True if there was something to restore. */
NIMBO_API bool nimbo_restore_displays(void);

/* ---------------------------------------------------------------------------------------
 * Settings file shared with macOS (core/settings.h), and its mapping to server options.
 * --------------------------------------------------------------------------------------- */

typedef struct {
    int cell_count;
    int cols[NIMBO_MAX_PANELS];
    int rows[NIMBO_MAX_PANELS];
    int text_size;  /* 0 (small text) .. 4 (very large) */
    float distance_m, gap_deg, curvature, size_deg;
    bool auto_start;
    float fov_y_deg, ipd_mm;
    bool stereo;
    float dead_zone_deg;
    bool gaze_cursor, dim_others, gather_windows;
    float k1, k2;
    bool onboarding_done;
    int tracking;   /* 0 both, 1 left/right only, 2 up/down only */
    int glasses_mode; /* 0 follow, 1 pinned, 2 steady */
    int brightness;   /* 1..8, 0 = leave as is */
    int pose_count; /* "panels": hand-placed monitors, same order as the cells; 0 = the grid */
    float pose_yaw[NIMBO_MAX_PANELS];
    float pose_pitch[NIMBO_MAX_PANELS];
    float pose_scale[NIMBO_MAX_PANELS];
} nimbo_settings;

NIMBO_API nimbo_settings nimbo_default_settings(void);
/* Missing file or keys: defaults. False only when the file exists but is not JSON. */
NIMBO_API bool nimbo_load_settings(const char* path, nimbo_settings* out);
NIMBO_API bool nimbo_save_settings(const char* path, const nimbo_settings* settings);
/* The arrangement follows hand-placed monitors: layout cells are derived from their positions
 * (core::cells_from_poses), so the cursor crosses to the neighbour you see. */
NIMBO_API void nimbo_options_from_settings(const nimbo_settings* settings, nimbo_options* out);

/* One-click layouts and usage profiles shared with macOS (core/presets.h). Ids are static
 * strings; titles stay in each app, keyed by id. */
typedef struct {
    const char* id; /* "curve", "dual", "triple" */
    int cell_count;
    int cols[NIMBO_MAX_PANELS];
    int rows[NIMBO_MAX_PANELS];
    float curvature, size_deg, gap_deg;
} nimbo_layout;

typedef struct {
    const char* id;        /* "code", "cinema" */
    const char* layout_id;
    float distance_m;
    int text_size;
    float curvature;       /* < 0: keep the layout's */
    float size_deg;        /* < 0: keep the layout's */
} nimbo_profile;

NIMBO_API int nimbo_layout_count(void);
NIMBO_API bool nimbo_layout_at(int index, nimbo_layout* out);
NIMBO_API int nimbo_profile_count(void);
NIMBO_API bool nimbo_profile_at(int index, nimbo_profile* out);

/* ---------------------------------------------------------------------------------------
 * Guided sensor -> head calibration (the app's wizard): bias on the table, then centre, up,
 * down, left, right. Not while a server runs (both need the glasses' IMU).
 * --------------------------------------------------------------------------------------- */

typedef enum {
    NIMBO_CALIB_BIAS = 0,
    NIMBO_CALIB_CENTER,
    NIMBO_CALIB_UP,
    NIMBO_CALIB_DOWN,
    NIMBO_CALIB_LEFT,
    NIMBO_CALIB_RIGHT,
} nimbo_calib_step;

typedef struct {
    nimbo_calib_step step;
    bool returning;      /* a direction was captured: back to centre */
    const char* hint;    /* "waiting for rest", "put the glasses on", "move", "further", "hold" */
    float rate_degs;
    float rotation_deg;
    bool bias_ok;
    float captured_deg;
} nimbo_calib_status;

typedef void (*nimbo_calib_fn)(void* ctx, const nimbo_calib_status* status);

typedef struct nimbo_calibration nimbo_calibration;
NIMBO_API nimbo_calibration* nimbo_calibration_create(void);
NIMBO_API void nimbo_calibration_destroy(nimbo_calibration* c);
/* Blocking; `fn` ~10 Hz from the calling thread. Writes `out_path` on success. Returns 0 on
 * success, 1 when the flow stopped (no glasses, timeout, cancel), 2 when the result was
 * rejected; `error` gets the reason. */
NIMBO_API int nimbo_calibration_run(nimbo_calibration* c, const char* out_path, nimbo_calib_fn fn, void* ctx,
                                    char* error, int error_size);
NIMBO_API void nimbo_calibration_cancel(nimbo_calibration* c); /* any thread */

#ifdef __cplusplus
}
#endif

#endif /* NIMBO_WIN_H */
