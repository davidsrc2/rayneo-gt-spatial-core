#!/usr/bin/env python3
"""Statistics of Nimbo's once-a-second render lines (TRACK / CALIB) in nimbo.log.

    scripts/log_stats.py [LOG] [--last N] [--since HH:MM:SS] [--until HH:MM:SS] [--skip S]

LOG defaults to ~/Library/Application Support/Nimbo/nimbo.log (macOS) or
%LOCALAPPDATA%/Nimbo/nimbo.log (Windows); stdout captured from the CLI works too. --last N
keeps the last N runs (a run starts at its "surface:" line); --skip S drops each run's first S
render lines (start-up). Prints Markdown tables: per mode (TRACK / CALIB) and per motion (a
line with "moving jitter" is moving, the rest still), mean and p95 of missed, late, latch,
sample->presented and moving jitter; the latch histogram; and per run the gyro bias at
"ready: tracking" and 10 s later, and the pose age before "glasses IMU stopped".
"""
import math
import os
import re
import sys

NUM = r"(-?[\d.]+)"
FIELDS = {
    "frames": re.compile(r"render +(\d+) fps"),
    "idle": re.compile(r"\((\d+) idle\)"),
    "capture": re.compile(r"capture +(\d+) fps"),
    "pose_age": re.compile(r"pose age " + NUM + "/" + NUM),
    "presented": re.compile(r"sample->(?:presented|scanout) " + NUM + "/" + NUM),
    "predicted": re.compile(r"predicted " + NUM + " ms"),
    "late": re.compile(r"\((?:late|target err) \+?" + NUM),
    "interval": re.compile(r"frame interval " + NUM + "(?:±|\\+-)" + NUM + " max " + NUM),
    "missed": re.compile(r"missed (\d+)(?: \(moving (\d+), still (\d+)(?:, latch (\d+))?(?:, switches (\d+))?\))?"),
    "latch": re.compile(r"latch " + NUM + " ms"),
    "latch_applied": re.compile(r"applied " + NUM),
    "cpu": re.compile(r"cpu " + NUM + "/" + NUM),
    "ready": re.compile(r"ready " + NUM + r"\)"),
    "jitter": re.compile(r"moving jitter " + NUM),
    "wobble": re.compile(r"still wobble " + NUM),
    "gpu": re.compile(r"gpu " + NUM + " ms"),
    "gpu_wait": re.compile(r"gpu " + NUM + r" ms \(wait " + NUM),
}
LINE = re.compile(r"^(?:(\d\d:\d\d:\d\d)\.\d+ )?(TRACK|CALIB) render")
STAMP = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ ")
BIAS = re.compile(r"imu: gyro bias " + NUM + " " + NUM + " " + NUM)


def default_log():
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA", ""), "Nimbo", "nimbo.log")
    return os.path.expanduser("~/Library/Application Support/Nimbo/nimbo.log")


def parse_line(text):
    m = LINE.match(text)
    if not m:
        return None
    row = {"time": m.group(1), "mode": m.group(2)}
    for key, rx in FIELDS.items():
        f = rx.search(text)
        if not f:
            continue
        if key == "missed":
            row["missed"] = int(f.group(1))
            if f.group(2) is not None:
                row["missed_moving"] = int(f.group(2))
                row["missed_still"] = int(f.group(3))
            if f.group(4) is not None:
                row["missed_latch"] = int(f.group(4))
            if f.group(5) is not None:
                row["switches"] = int(f.group(5))
        elif key in ("pose_age", "presented", "cpu"):
            row[key] = float(f.group(1))
            row[key + "_max"] = float(f.group(2))
        elif key == "interval":
            row["interval"], row["interval_std"], row["interval_max"] = map(float, f.groups())
        elif key in ("frames", "idle", "capture"):
            row[key] = int(f.group(1))
        else:
            row[key] = float(f.group(1))
    row.setdefault("idle", 0)
    row["moving"] = "jitter" in row
    return row


def read_runs(path):
    runs, cur = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            text = raw.rstrip("\n")
            if "surface:" in text or cur is None:
                cur = {"rows": [], "events": []}
                runs.append(cur)
            row = parse_line(text)
            if row:
                cur["rows"].append(row)
            else:
                cur["events"].append((len(cur["rows"]), text))
    return [r for r in runs if r["rows"] or r["events"]]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def p95(v):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(0.95 * len(s)) - 1))]


def fmt(x, digits=1):
    return "–" if x != x else f"{x:.{digits}f}"


def in_window(t, since, until):
    if t is None:
        return True
    return (since is None or t >= since) and (until is None or t <= until)


def table(rows):
    out = ["| mode | lines | missed mean | missed p95 | lines > 30 | missed/s while moving | late mean | late p95 | "
           "latch mean | latch p95 | applied mean | applied p95 | s->presented mean | s->presented p95 | "
           "predicted mean | jitter mean | jitter p95 | wobble mean | missed by latch | cpu mean | cpu max | "
           "GPU done before deadline | late switches |",
           "|" + "---|" * 23]
    for mode in ("TRACK", "CALIB"):
        for label, sel in (("all", lambda r: True), ("moving", lambda r: r["moving"]), ("still", lambda r: not r["moving"])):
            rs = [r for r in rows if r["mode"] == mode and sel(r)]
            if not rs:
                continue
            drawn = [r for r in rs if r["frames"] > 0]  # late, latency: only seconds with frames drawn
            miss = [r["missed"] for r in rs if "missed" in r]
            split = [r["missed_moving"] for r in rs if "missed_moving" in r and r["moving"]]
            g = lambda k, src=drawn: [r[k] for r in src if k in r]
            out.append("| " + " | ".join([
                f"{mode} {label}", str(len(rs)), fmt(mean(miss)), fmt(p95(miss), 0), str(sum(1 for m in miss if m > 30)),
                fmt(mean(split)) if split else "–",
                fmt(mean(g("late"))), fmt(p95(g("late"))), fmt(mean(g("latch", rs))), fmt(p95(g("latch", rs))),
                fmt(mean(g("latch_applied"))), fmt(p95(g("latch_applied"))),
                fmt(mean(g("presented"))), fmt(p95(g("presented"))), fmt(mean(g("predicted"))),
                fmt(mean(g("jitter")), 3), fmt(p95(g("jitter")), 3), fmt(mean(g("wobble")), 4),
                str(sum(g("missed_latch", rs))) if g("missed_latch", rs) else "–",
                fmt(mean(g("cpu"))), fmt(max(g("cpu_max")) if g("cpu_max") else float("nan")),
                fmt(mean(g("ready"))), str(sum(g("switches", rs))) if g("switches", rs) else "–"]) + " |")
    return "\n".join(out)


def latch_histogram(rows):
    hist = {}
    for r in rows:
        if "latch" in r:
            b = round(r["latch"])
            hist[b] = hist.get(b, 0) + 1
    if not hist:
        return ""
    out = ["| latch (ms, rounded) | lines |", "|---|---|"]
    out += [f"| {b} | {hist[b]} |" for b in sorted(hist)]
    capped = sum(1 for r in rows if r.get("latch") == 12.0)
    return "\n".join(out) + f"\n\nlines at exactly 12.0 ms: {capped}"


def run_events(runs):
    out = []
    for i, run in enumerate(runs):
        ready = None
        for k, (row_i, text) in enumerate(run["events"]):
            if "ready: tracking" in text:
                ready = k
            b = BIAS.search(text)
            if b and ready is not None:
                biases = [BIAS.search(t) for _, t in run["events"][ready:] if BIAS.search(t)]
                if biases:
                    first = biases[0].groups()
                    later = biases[1].groups() if len(biases) > 1 else None
                    stamp = STAMP.match(run["events"][ready][1])
                    out.append(f"- run {i + 1} ready {stamp.group(1) if stamp else ''}: bias at ready "
                               f"{'/'.join(first)} deg/s" + (f", 10 s later {'/'.join(later)}" if later else ""))
                ready = None
            if "glasses IMU stopped" in text:
                ages = [r.get("pose_age", 0) for r in run["rows"][max(0, row_i - 3):row_i]]
                out.append(f"- run {i + 1}: IMU stopped after pose age up to {fmt(max(ages) if ages else float('nan'))} ms")
    return "\n".join(out)


def main(argv):
    path, last, since, until, skip = None, None, None, None, 0
    it = iter(argv[1:])
    for a in it:
        if a == "--last":
            last = int(next(it))
        elif a == "--since":
            since = next(it)
        elif a == "--until":
            until = next(it)
        elif a == "--skip":
            skip = int(next(it))
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            path = a
    path = path or default_log()
    runs = read_runs(path)
    if last:
        runs = runs[-last:]
    rows = [r for run in runs for r in run["rows"][skip:] if in_window(r["time"], since, until)]
    if not rows:
        print("no TRACK / CALIB lines")
        return 1
    print(f"{path}: {len(runs)} run(s), {len(rows)} render lines\n")
    print(table(rows))
    print()
    print(latch_histogram(rows))
    ev = run_events(runs)
    if ev:
        print("\n" + ev)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
