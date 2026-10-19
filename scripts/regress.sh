#!/usr/bin/env bash
# regress.sh [RECORDINGS_DIR]: every IMU recording in the library (default ~/Library/Application
# Support/Nimbo/recordings/*.imu), checked twice:
#   1. offline (build/replay_eval): prediction error while moving at the app's 16 ms;
#   2. on the glasses (they must be connected): replayed for its length with the capture load on
#      both monitors (build/capture_load), then scripts/log_stats.py on its TRACK lines.
# Fails (exit 1) when a threshold below is crossed; the per-recording logs and tables stay in
# build/regress/. Run before a release; CI cannot (no glasses).
#   cmake --build build && swiftc -O scripts/capture_load.swift -o build/capture_load
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
lib="${1:-$HOME/Library/Application Support/Nimbo/recordings}"
out="$root/build/regress"
calib="$HOME/Library/Application Support/Nimbo/calibration.json"
mkdir -p "$out"

# Thresholds (the measurements of October 2026 with some headroom).
MAX_PRED_ERR_DEG=0.70        # rms prediction error while moving, offline
MAX_MISSED_MOVING=5.0        # missed refreshes per second while moving
MAX_SECONDS_OVER_30=3        # seconds with more than 30 missed
MAX_LATCH_P95_MS=4.0
MAX_PRESENTED_MS=23.0        # sample->presented, mean
MAX_STILL_WOBBLE=0.0030      # deg/frame, mean

fail=0
shopt -s nullglob
recs=("$lib"/*.imu)
[[ ${#recs[@]} -eq 0 ]] && { echo "no recordings in $lib"; exit 1; }
for rec in "${recs[@]}"; do
    name="$(basename "$rec" .imu)"
    map=(); [[ -f "$rec.biasmap.json" ]] && map=(--map "$rec.biasmap.json")
    err=$("$root/build/replay_eval" "$rec" --calib "$calib" "${map[@]}" --horizons 16 | awk '/constant w 16/ {print $4}')
    # Length of the recording from its sample count (457 Hz).
    samples=$("$root/build/replay_eval" "$rec" --horizons 0 | head -1 | grep -oE '[0-9]+ samples' | grep -oE '[0-9]+')
    secs=$(( samples / 457 + 4 ))
    (sleep 3; "$root/build/capture_load" $((secs - 5)) >/dev/null) &
    "$root/scripts/session.sh" "$secs" --replay-imu "$rec" > "$out/$name.out" 2>&1
    wait
    python3 "$root/scripts/log_stats.py" "$out/$name.out" --skip 2 > "$out/$name.md"
    read -r missed over latch presented < <(awk -F'|' '/^\| TRACK moving/ {gsub(/ /,""); print $4, $6, $11, $14}' "$out/$name.md")
    wobble=$(awk -F'|' '/^\| TRACK still/ {gsub(/ /,""); print $19}' "$out/$name.md")
    verdict=ok
    # No reading is a failure (the run did not draw: no glasses, a crash), "–" only when the
    # table says there was nothing of that kind to measure.
    check() {
        if [[ -z "$1" ]]; then verdict=FAIL; fail=1; echo "  $name: no $3 (see $out/$name.out)"; return; fi
        LC_ALL=C awk -v v="$1" -v m="$2" 'BEGIN { exit !(v == "–" || v + 0 <= m + 0) }' || { verdict=FAIL; fail=1; echo "  $name: $3 $1 > $2"; }
    }
    check "$err" $MAX_PRED_ERR_DEG "prediction error"
    check "$missed" $MAX_MISSED_MOVING "missed/s moving"
    check "$over" $MAX_SECONDS_OVER_30 "seconds > 30 missed"
    check "$latch" $MAX_LATCH_P95_MS "latch p95"
    check "$presented" $MAX_PRESENTED_MS "sample->presented"
    check "$wobble" $MAX_STILL_WOBBLE "still wobble"
    echo "$name: $verdict (pred err $err deg, missed $missed/s moving, >30: $over, latch p95 $latch ms, s->p $presented ms, wobble $wobble)"
done
exit $fail
