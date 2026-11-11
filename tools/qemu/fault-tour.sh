#!/bin/bash
# Inject every fault into the running Tether stack in the VM, one after another,
# and print what the arbiter did. Needs the stack running (it starts with the
# image; or tools/qemu/stack.sh start) and the VM up (tools/qemu/run-vm.sh start).
# crash and hang are recovered by the supervisor, tetherd, not by this script.
#   tools/qemu/fault-tour.sh [fault ...]     default: all
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FAULTS=("$@"); [ ${#FAULTS[@]} -gt 0 ] || FAULTS=(spike nan drift stuck slow bypass hog crash hang)

exec "$ROOT/tools/qemu/run-vm.sh" ssh "FAULTS='${FAULTS[*]}' sh -s" <<'REMOTE'
cd /data/tether
C=bin/tether-ctl
pick() { echo "$J" | tr ',' '\n' | grep -E "\"$1\"" | head -1 | sed 's/.*://; s/[{}"]//g'; }
show() {
  J=$($C status 2>/dev/null)
  [ -n "$J" ] || { echo "   (arbiter not answering)"; return; }
  echo "   mode=$(pick mode) reason=$(pick reason) vt_last=$(pick vt_last) switches_to_safe=$(pick switches_to_safe)" \
       "violations=$(pick violations) ai_connected=$(pick ai_connected) ai_restarts=$(pick ai_restarts)" \
       "refused_by_os=$(pick hw_denied_opens) hw_trips=$(pick hw_watchdog_trips)"
  echo "   wake p99=$(( $(pick p99_ns | head -1) / 1000 )) us, max=$(( $(pick max_ns | head -1) / 1000 )) us, overruns=$(pick ctl_overruns)"
}
echo "== baseline"; show
for f in $FAULTS; do
  echo; echo "== fault: $f"
  $C fault $f
  sleep 7
  show
  case $f in
    crash|hang)
      echo "   waiting for tetherd to bring the AI back"
      sleep 8; show ;;
    *) $C fault none; sleep 6; echo "   after the fault clears:"; show ;;
  esac
done
REMOTE
