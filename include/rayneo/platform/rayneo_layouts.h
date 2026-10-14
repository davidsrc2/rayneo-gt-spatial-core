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

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_LAYOUTS_H */
