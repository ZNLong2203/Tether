#!/bin/bash
# Start, stop or inspect the Tether processes inside the VM (temporary stand-in
# for the supervisor, tetherd). Shared options live in /data/tether/flags.
#   tools/qemu/stack.sh start | stop | status | logs
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# What QEMU's software emulation can sustain: see docs on timing. A real target
# would use the defaults (control 1000 Hz, AI 100 Hz, freshness 25 ms, watchdog 50 ms).
FLAGS="${TETHER_FLAGS:---control-hz=250 --ai-hz=50 --plant-hz=500 --readmit-ticks=50 --max-age-ms=60 --hw-watchdog-ms=120}"
vm() { "$ROOT/tools/qemu/run-vm.sh" ssh "$@"; }

case "${1:-}" in
  start)
    vm "
      for n in tether-ai tether-arbiter tether-hw; do slay -f \$n 2>/dev/null; done; sleep 1
      cd /data/tether; echo '$FLAGS' > flags
      bin/tether-hw $FLAGS > hw.log 2>&1 &
      sleep 1
      bin/tether-arbiter $FLAGS > arb.log 2>&1 &
      sleep 1
      bin/tether-ai $FLAGS > ai.log 2>&1 &
      sleep 1; echo started" ;;
  stop)   vm 'for n in tether-ai tether-arbiter tether-hw; do slay -f $n 2>/dev/null; done; sleep 1; echo stopped' ;;
  status) vm 'pidin -F "%a %N" 2>&1 | grep tether- | sort -u; /data/tether/bin/tether-ctl hw' ;;
  logs)   vm 'cd /data/tether; for f in hw arb ai; do echo "== $f.log"; tail -${2:-8} $f.log; done' ;;
  *) echo "usage: $0 start|stop|status|logs"; exit 2 ;;
esac
