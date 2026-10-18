# Glasses surface straight to the display (macOS)

Why it matters: a surface WindowServer composites reaches the glasses a refresh later than one
it flips directly to the display, and the switch between the two shows as a step in the
panels. RealWarp on Windows checks that its swap chain presents as `Independent_Flip`; this is
the macOS counterpart.

## Conditions Nimbo sets

| Condition | Where |
|---|---|
| Opaque `CAMetalLayer` (`isOpaque = true`) and opaque view / window | `MetalView`, the render window setup in `SpatialServer` |
| Borderless window exactly the glasses display's frame | render window setup in `SpatialServer` |
| `drawableSize` = the display mode's native pixels (1920x1080) | render window setup, `retargetGlasses` |
| No EDR (`wantsExtendedDynamicRangeContent = false`), no colour space (`colorspace = nil`) | render window setup in `SpatialServer` |
| `displaySyncEnabled = true` | render window setup in `SpatialServer` |
| Glasses not mirrored (`--unmirror`, the app always) | `Displays` |
| Nothing above the window; the cursor drawn inside the render | gaze cursor, `logCursorState` |
| ScreenCaptureKit at 60 fps per source (each captured frame is WindowServer GPU work) | `startCaptures` |

Three seconds after start the log says which of them hold:

```
surface: direct to display possible (opaque, native size, no EDR / colour space, vsync, nothing above)
surface: direct to display prevented: layer not opaque; window above: ...
```

WindowServer's cursor windows (one per display, named "Cursor") are listed apart: they are
the hardware cursor plane, not proven to force compositing.

## Checking it with Instruments (needs Xcode)

1. `xcrun xctrace record --template 'Metal System Trace' --attach Nimbo --time-limit 20s --output nimbo.trace`
   (or Instruments.app -> Metal System Trace -> target Nimbo), while turning the head.
2. Open the trace. In the **Display** track of the glasses display, each surface shown is a
   bar; select one: a direct surface is the app's own drawable (`CAMetalLayer` / IOSurface of
   Nimbo) going straight to the display, a composited one appears as a WindowServer surface fed
   by Nimbo's drawable a frame earlier.
3. In the **GPU** track, a WindowServer compositing pass for every glasses vsync means
   composited; none means direct.
4. Compare with the `TRACK` lines of the same seconds: composited seconds show `late` ~8.3 ms
   at 120 Hz with `frame interval 8.3±0.0`.

Not run yet: the reference Mac has the Command Line Tools only (no Xcode, no `xctrace`).

## Equivalent metric (what the measurements use)

In the `TRACK` line: frames presented a refresh or more after their target although their CPU
work was done in time (`missed N (moving M, still S, latch K)`: N - K), with an even rhythm
(`frame interval 8.3±0.0`). That is a whole extra refresh between the commit and the photons,
the signature of compositing (or of the frame's GPU end missing WindowServer's latch).
`scripts/log_stats.py` reports it per motion; `ready` (GPU end before the display link's
deadline) tells the two apart.

Measured on the reference recording (see the commit history, phase 3): the late state starts
after seconds of drawing every refresh (head turning, no idle frames) and ends at the first
idle frames; while in it the GPU ends 0.5-2 ms after the display link's `targetTimestamp`
(which is not WindowServer's real cutoff: on-time runs end 2 ms after it too). With
supersample 1 the GPU takes 0.5 ms instead of ~2 and the state did not appear.
