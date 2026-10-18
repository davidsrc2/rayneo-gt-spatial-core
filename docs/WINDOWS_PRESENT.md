# Glasses swap chain straight to the display (Windows)

The macOS counterpart is [DIRECT_TO_DISPLAY.md](DIRECT_TO_DISPLAY.md). A frame DWM composites
reaches the glasses a refresh later than one flipped to the display; RealWarp checks that its
swap chain presents as `Independent_Flip`.

## What the server does

| Item | Where |
|---|---|
| Flip model swap chain (`FLIP_DISCARD`, 2 buffers, frame latency waitable, latency 1) | `Server::create_swap_chain` |
| Borderless window exactly the glasses output | `Server::create_window` |
| Late latching with the margin from `core/latch_controller` (work = CPU to Present + GPU time) | `Server::frame`, `collect_presented` |
| GPU time per frame from `D3D11_QUERY_TIMESTAMP` pairs, read back without waiting | `Server::collect_gpu_times` |
| Presentation mode per frame statistics read (`IDXGISwapChainMedia::GetFrameStatisticsMedia`) | `Server::collect_presented` |
| Frames drawn on for 0.5 s after the head stops (fewer on-time <-> late switches) | `Server::frame` |
| The frame loop at `THREAD_PRIORITY_HIGHEST` | `Server::run` |

The `TRACK` line (same fields as macOS; `scripts/log_stats.py` reads both):

```
TRACK render 120 fps | ... | missed 3 (moving 3, still 0, latch 0, switches 2) | latch 2.0 ms (applied 1.9, cpu 0.4/0.9) | gpu 1.2 ms | present composed 0, overlay 0, direct 118
```

`present` counts `DXGI_FRAME_PRESENTATION_MODE` over the second: `composed` (DWM),
`overlay` (a hardware overlay plane), `direct` (`..._NONE`: not composed). Microsoft documents
the field for overlay-capable swap chains; on others it may stay at `composed` or fail. The
reliable check is PresentMon below.

## Checking it with PresentMon

1. Download PresentMon (https://github.com/GameTechDev/PresentMon/releases), console version.
2. With Nimbo running and the head turning:
   `PresentMon-*.exe --process_name nimbo_win.exe --timed 20 --output_file nimbo.csv`
   (or the process hosting `nimbo.dll`: `NimboApp.exe`).
3. In `nimbo.csv`, column `PresentMode`: `Hardware: Independent Flip` (or `Hardware Composed:
   Independent Flip`) is direct; `Composed: Flip` means DWM composites it. `MsBetweenDisplayChange`
   should read ~8.33 at 120 Hz, `MsUntilDisplayed` is the frame's latency after Present.
4. Compare with the `TRACK` lines of the same seconds (`present`, `missed`, `late`).

Not run yet: no Windows machine with the glasses in this project's measurements so far.
