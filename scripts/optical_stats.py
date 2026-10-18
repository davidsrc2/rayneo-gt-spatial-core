#!/usr/bin/env python3
"""What the glasses showed, from scripts/optical_motion.swift's CSV: how still and how smooth.

    scripts/optical_stats.py RUN.csv [RUN2.csv ...] [--fov 23] [--from S] [--to S]

Per run (pixels to degrees with the optics' vertical field of view over 1080 px, the same for
every app on the same glasses):
  still wobble   rms image shift per frame while the head is still (deg/frame)
  drift          net image motion over still stretches of 2 s or more (deg/s): panels staying put
  moving jitter  rms change of the per-frame step while turning (deg): uneven motion, vibration
  repeats        share of turning frames that barely moved between two that did: a frame shown
                 twice (a stutter)
Frames without texture (looking off the panels, or content changing) are left out, and the
steps across a frame the capture dropped (ScreenCaptureKit delivers ~100 of 120 a second): a
dropped capture is not a stutter on the glasses.
"""
import csv
import math
import sys


def load(path, t_from, t_to):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            t = float(r["t"])
            if t < t_from or t > t_to:
                continue
            ok = r["status"] == "idle" or float(r["peak"]) >= 0.15
            rows.append((t, float(r["dx"]), float(r["dy"]), ok))
    return rows


def stats(rows, deg_per_px):
    n = len(rows)
    ts = [r[0] for r in rows]
    gaps = sorted(b - a for a, b in zip(ts, ts[1:]))
    period = gaps[len(gaps) // 4] if gaps else 1 / 120  # the refresh (most captures are one apart)
    one = lambda i: 0 < i < n and abs((ts[i] - ts[i - 1]) - period) < 0.3 * period
    sp = [math.hypot(dx, dy) if ok else float("nan") for _, dx, dy, ok in rows]
    # Smoothed speed over +-6 frames tells still from moving (head speed ~1 deg/s at the edge).
    smooth = []
    for i in range(n):
        w = [v for v in sp[max(0, i - 6):i + 7] if v == v]
        smooth.append(sum(w) / len(w) if w else float("nan"))
    still_px = 1.0 / 120 / deg_per_px  # 1 deg/s at 120 Hz, in px per frame
    wob, jit, moving, repeats = [], [], 0, 0
    drift_segments = []
    seg_start, seg_sum = None, [0.0, 0.0]
    for i, (t, dx, dy, ok) in enumerate(rows):
        s = smooth[i]
        is_still = ok and s == s and s < still_px
        if is_still:
            wob.append(sp[i])
            if seg_start is None:
                seg_start, seg_sum = t, [0.0, 0.0]
            seg_sum[0] += dx
            seg_sum[1] += dy
        else:
            if seg_start is not None and t - seg_start >= 2.0:
                drift_segments.append((t - seg_start, math.hypot(*seg_sum)))
            seg_start = None
        if ok and s == s and s >= 3 * still_px and one(i) and one(i - 1) and rows[i - 1][3]:
            moving += 1
            pdx, pdy = rows[i - 1][1], rows[i - 1][2]
            j = math.hypot(dx - pdx, dy - pdy)
            if j < 2.0 / deg_per_px:
                jit.append(j)
            nxt = rows[i + 1] if i + 1 < n else None
            if nxt and nxt[3] and one(i + 1) and sp[i] < 0.25 * s and math.hypot(pdx, pdy) > 0.5 * s and math.hypot(nxt[1], nxt[2]) > 0.5 * s:
                repeats += 1
    if seg_start is not None and rows and rows[-1][0] - seg_start >= 2.0:
        drift_segments.append((rows[-1][0] - seg_start, math.hypot(*seg_sum)))
    rms = lambda v: math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")
    still_s = sum(d for d, _ in drift_segments)
    return {
        "frames": n,
        "still s": still_s,
        "still wobble deg/frame": rms(wob) * deg_per_px,
        "drift deg/s": (sum(m for _, m in drift_segments) / still_s * deg_per_px) if still_s else float("nan"),
        "moving frames": moving,
        "moving jitter deg": rms(jit) * deg_per_px,
        "repeats %": 100.0 * repeats / moving if moving else float("nan"),
    }


def main(argv):
    fov, t_from, t_to, paths = 23.0, 0.0, 1e9, []
    it = iter(argv[1:])
    for a in it:
        if a == "--fov":
            fov = float(next(it))
        elif a == "--from":
            t_from = float(next(it))
        elif a == "--to":
            t_to = float(next(it))
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    deg_per_px = fov / 1080.0
    keys = None
    for p in paths:
        s = stats(load(p, t_from, t_to), deg_per_px)
        if keys is None:
            keys = list(s)
            print("| run | " + " | ".join(keys) + " |")
            print("|---|" + "---|" * len(keys))
        print(f"| {p.rsplit('/', 1)[-1]} | " + " | ".join(
            f"{v:.4f}" if isinstance(v, float) and v < 1 else f"{v:.1f}" if isinstance(v, float) else str(v) for v in s.values()) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
