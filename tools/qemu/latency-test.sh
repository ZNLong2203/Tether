#!/bin/bash
# Measure the control loop's wake-up latency in separate windows, so the effect
# of each stress shows up on its own instead of being averaged away.
#   tools/qemu/latency-test.sh [seconds-per-window] [window ...]
# A window is a fault name (none, hog, bypass, slow, ...). Default: none hog bypass slow.
# Repeat names to repeat a window, e.g.  latency-test.sh 15 none bypass none bypass
# Needs the stack running (tools/qemu/stack.sh start).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECS="${1:-20}"; [ $# -gt 0 ] && shift
WINDOWS=("$@"); [ ${#WINDOWS[@]} -gt 0 ] || WINDOWS=(none hog bypass slow)
exec "$ROOT/tools/qemu/run-vm.sh" ssh "SECS=$SECS WINDOWS='${WINDOWS[*]}' sh -s" <<'REMOTE'
cd /data/tether
C=bin/tether-ctl
J=
# first/last match of a key: wake and exec both have a p99_ns, wake comes first
first() { echo "$J" | tr ',' '\n' | grep -E "\"$1\"" | head -1 | sed 's/.*://; s/[{}"]//g'; }
last()  { echo "$J" | tr ',' '\n' | grep -E "\"$1\"" | tail -1 | sed 's/.*://; s/[{}"]//g'; }
for w in $WINDOWS; do
  $C fault none >/dev/null; sleep 5; $C reset; [ "$w" != none ] && $C fault "$w" >/dev/null
  sleep $SECS
  J=$($C status 2>/dev/null)
  printf "%-8s n=%-6s wake us: p50=%-5s p99=%-5s p99.9=%-6s max=%-6s | exec us: p99=%-5s max=%-6s | overruns=%s violations=%s mode=%s\n" \
    "$w" "$(first count)" "$(( $(first p50_ns) / 1000 ))" "$(( $(first p99_ns) / 1000 ))" "$(( $(first p999_ns) / 1000 ))" \
    "$(( $(first max_ns) / 1000 ))" "$(( $(last p99_ns) / 1000 ))" "$(( $(last max_ns) / 1000 ))" \
    "$(first ctl_overruns)" "$(first violations)" "$(first mode)"
done
$C fault none >/dev/null
REMOTE
