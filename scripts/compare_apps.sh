#!/usr/bin/env bash
# compare_apps.sh [--realwarp]: Nimbo (and with --realwarp, RealWarp after it, for glasses it
# supports; RealWarp 0.38 does not drive the RayNeo GT) measured the same way, from what the glasses are sent
# (scripts/optical_motion.swift): spoken instructions (Spanish), two phases per app:
#   1. glasses still on the table (40 s): wobble and drift with no head at all;
#   2. worn (90 s): still looking ahead, then looking from monitor to monitor, then still.
# Every screen but the glasses shows the same still noise texture (capture_load --static --all),
# so both apps draw the same content. Results in build/compare/, table at the end
# (scripts/optical_stats.py). Nimbo first (the build in build/), then RealWarp
# (/Applications/RealWarp.app, started by you when asked: its own interface).
#   swiftc -O scripts/optical_motion.swift -o build/optical_motion
#   swiftc -O scripts/capture_load.swift -o build/capture_load
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/compare"
mkdir -p "$out"
voice() { say -v Eddy "$1" 2>/dev/null || say "$1"; }
step() { echo "[$(date +%H:%M:%S)] $1"; voice "$1"; }

measure() {  # NAME: the table and worn captures of whatever drives the glasses now
    local name="$1"
    (sleep 1; "$root/build/capture_load" 200 --static --all >/dev/null) &
    local load=$!
    step "Deja las gafas quietas sobre la mesa, sin tocarlas, cuarenta segundos."
    sleep 8
    "$root/build/optical_motion" 40 "$out/$name-table.csv" || echo "optical_motion failed (is the glasses window capturable?)"
    step "Ahora ponte las gafas, mira al frente y recentra."
    sleep 15
    step "Quieto, mirando al frente, treinta segundos."
    ( sleep 30; step "Mira de un monitor a otro, a ritmo normal, cuarenta segundos."
      sleep 40; step "Quieto otra vez, mirando al frente." ) &
    local cues=$!
    "$root/build/optical_motion" 90 "$out/$name-worn.csv" || echo "optical_motion failed"
    wait "$cues"
    kill "$load" 2>/dev/null
    wait "$load" 2>/dev/null
}

pgrep -f "Nimbo.app/Contents/MacOS/Nimbo" >/dev/null && { echo "quit Nimbo first"; exit 1; }
pgrep -x RealWarp >/dev/null && { echo "quit RealWarp first"; exit 1; }

step "Prueba de Nimbo. Deja las gafas en la mesa mientras arranca."
"$root/scripts/session.sh" 230 --capturable > "$out/nimbo.out" 2>&1 &
nimbo=$!
sleep 20  # displays arranged, the IMU's start-up calibration with the glasses on the table
measure nimbo
kill "$nimbo" 2>/dev/null
wait "$nimbo" 2>/dev/null
sleep 5

if [[ "${1:-}" != "--realwarp" ]]; then
    step "Terminado. Gracias."
    python3 "$root/scripts/optical_stats.py" "$out/nimbo-table.csv" "$out/nimbo-worn.csv" | tee "$out/table.md"
    exit 0
fi

step "Prueba de RealWarp. Deja las gafas en la mesa, abre RealWarp y actívalo como lo usas normalmente. Tienes cuarenta y cinco segundos."
open -a RealWarp
sleep 45
measure realwarp
osascript -e 'tell application "RealWarp" to quit' >/dev/null 2>&1
step "Terminado. Gracias."

python3 "$root/scripts/optical_stats.py" "$out/nimbo-table.csv" "$out/realwarp-table.csv" "$out/nimbo-worn.csv" "$out/realwarp-worn.csv" | tee "$out/table.md"
