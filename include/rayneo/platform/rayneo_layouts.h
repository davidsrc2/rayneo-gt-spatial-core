#ifndef RAYNEO_LAYOUTS_H
#define RAYNEO_LAYOUTS_H

/*
 * One-click layouts and usage profiles (core/presets.h) over a C ABI, so the macOS app
 * (Swift) and the Windows app (C# P/Invoke) offer the same choices. Titles are UI strings
 * and stay in each app, keyed by id. Strings are static: never freed.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAYNEO_MAX_PRESET_CELLS 8

typedef struct {
    const char* id;
    int cell_count;
    int cols[RAYNEO_MAX_PRESET_CELLS];
    int rows[RAYNEO_MAX_PRESET_CELLS]; /* 0 = top */
    float curvature;                   /* 0 flat .. 1 cylinder */
    float arc_deg;                     /* panel width */
    float gap_deg;
} rayneo_layout_preset;

typedef struct {
    const char* id;
    const char* layout_id;
    float distance_m;
    int text_size;   /* index into the app's text-size steps */
    float curvature; /* < 0: keep the layout's */
    float arc_deg;   /* < 0: keep the layout's */
} rayneo_profile;

int rayneo_layout_preset_count(void);
bool rayneo_layout_preset_at(int index, rayneo_layout_preset* out);
int rayneo_profile_count(void);
bool rayneo_profile_at(int index, rayneo_profile* out);

/*
 * Editing an arrangement (core/presets.h), the same in both apps. Cells are parallel arrays
 * (row 0 = top), poses parallel arrays in degrees (yaw right, pitch up) with their scale;
 * at most RAYNEO_MAX_PRESET_CELLS of either.
 */
bool rayneo_next_free_cell(const int* cols, const int* rows, int count, int* col, int* row);
/* Monitor `index` to (col, row), swapping with one there; false and untouched when invalid. */
bool rayneo_move_cell(int* cols, int* rows, int count, int index, int col, int row);
bool rayneo_pose_overlaps(const float* yaw, const float* pitch, const float* scale, int count, int index,
                          float arc_deg, float aspect);
void rayneo_pose_beside(const float* yaw, const float* pitch, const float* scale, int count, float arc_deg,
                        float aspect, float gap_deg, float* out_yaw, float* out_pitch);

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_LAYOUTS_H */
