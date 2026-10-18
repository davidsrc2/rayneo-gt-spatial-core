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

`scripts/trace_direct.sh [SECONDS]` does it from the command line (Nimbo running): it records
the trace and prints, from its `displayed-surfaces-interval` table, per display whether the
surfaces shown were direct and why not (`detachment-reason`), and the CPU-to-display latency of
the glasses' frames. If every display table comes out empty, macOS stopped emitting display
events (seen after several display reconfigurations in a row): reconnect the glasses or restart.

## Result (Instruments, 18-10-2026, reference MacBook Pro M4, macOS 27.2, glasses at 120 Hz)

- Nimbo (replay of the reference recording, capture load): of 4645 surfaces shown on the
  glasses display in 60 s, none direct to display. What the display shows are WindowServer's
  own framebuffers, filled from Nimbo's drawables: composited every frame. CPU-to-display
  latency 20-21 ms (3686 frames) or 29 ms (646 frames): the late state is one more refresh of
  that composition.
- Variants that did not change it (all "No"): window level normal or floating instead of
  screen saver, native full screen, layer colour space set to the display's or sRGB, a 10-bit
  drawable (bgr10a2Unorm), window shared with capture, system cursor hidden.
- A minimal app (one opaque full-screen CAMetalLayer clearing to a colour, nothing else, no
  capture) on the glasses display: also never direct, and WindowServer gives the reason
  `display rejected` (8-bit and 10-bit drawables, borderless and native full screen). The
  glasses' display does not take a detached surface on this Mac: the composition is not
  Nimbo's doing and Nimbo cannot avoid it. Its framebuffer is in a format of its own (`83b&`)
  while the drawables are 8-bit BGRA.

What follows for the late state: it is WindowServer's compositor taking a frame one refresh
later, not a missed deadline of ours; the prediction already follows it (fixed + lateness) and
drawing on through short pauses keeps the switches between the two states rare.

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
(which is not WindowServer's real cutoff: on-time runs end 2 ms after it too). The GPU's mean
time per frame is ~1.1 ms with supersample 2 (the "~2 ms" first quoted was one frame a second)
and its wait from the commit 0.7-0.8 ms, the same in on-time and late seconds: neither tells the
late state apart. Drawing on for 0.5 s after the head stops cut the switches between the two
states 113 -> 10 in a replay where it appeared. Windows: [WINDOWS_PRESENT.md](WINDOWS_PRESENT.md).
