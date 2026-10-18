#!/usr/bin/env bash
# session.sh SECONDS [nimbo args...]: Nimbo from the CLI configured as the app runs it (the space
# at real size, two monitors), for measurements: add --record-imu FILE to record the head's
# motion, --replay-imu FILE to play one back. Uses build/Nimbo.app.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
d="$1"; shift
exec "$root/build/Nimbo.app/Contents/MacOS/Nimbo" --layout "0,0;1,0" --vd-size 1024x576 \
  --distance 1.548 --gap 1 --arc 38 --tilt 1 --fov 23 --late-latch --prediction-ms 16 --supersample 2 \
  --dead-zone -2 --no-gaze-cursor --track both --glasses-mode follow --brightness 8 --real-size --unmirror \
  --calib "$HOME/Library/Application Support/Nimbo/calibration.json" --duration "$d" "$@"
