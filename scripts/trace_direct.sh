#!/usr/bin/env bash
# trace_direct.sh [SECONDS]: Metal System Trace of the running Nimbo (needs Xcode, its license
# accepted), then per display whether the surfaces shown went straight to it
# (direct-to-display) and why not (detachment-reason), and the glasses frames' CPU-to-display
# latency (a refresh more = composited one frame late). Trace in build/trace_direct.trace.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
pid=$(pgrep -f "Nimbo.app/Contents/MacOS/Nimbo" | head -1) || { echo "Nimbo is not running"; exit 1; }
out="$root/build/trace_direct"
rm -rf "$out.trace"
xcrun xctrace record --template 'Metal System Trace' --attach "$pid" --time-limit "${1:-15}s" --output "$out.trace" >/dev/null
xcrun xctrace export --input "$out.trace" \
    --xpath '/trace-toc/run[@number="1"]/data/table[@schema="displayed-surfaces-interval"]' > "$out.xml"
echo "surfaces shown, per display / direct / reason:"
python3 "$root/scripts/xctrace_table.py" "$out.xml" display-name direct-to-display detachment-reason
echo "CPU to display latency of the frames shown (ms, rounded):"
python3 -c "
import sys, collections; sys.path.insert(0, '$root/scripts')
from xctrace_table import rows
c = collections.Counter(round(float(r['cpu-to-display-latency'].split()[0])) for r in rows('$out.xml') if r['cpu-to-display-latency'] and not r['display-name'].startswith('Built-In'))
print(sorted(c.items()))"
