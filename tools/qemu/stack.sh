#!/bin/bash
# Start, stop or inspect the Tether processes inside the VM, through the
# supervisor tetherd. Shared options live in /data/tether/flags.
#   tools/qemu/stack.sh start | stop | status | logs
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# What QEMU's software emulation can sustain: see docs on timing. A real target
# would use the defaults (control 1000 Hz, AI 100 Hz, freshness 25 ms, watchdog 50 ms).
FLAGS="${TETHER_FLAGS:-$(cat "$ROOT/qnx/image/tether.flags")}"
vm() { "$ROOT/tools/qemu/run-vm.sh" ssh "$@"; }

case "${1:-}" in
  start)
    vm "
      slay -f tetherd 2>/dev/null; for n in tether-bridge tether-ai tether-arbiter tether-hw; do slay -f \$n 2>/dev/null; done; sleep 1
      cd /data/tether; echo '$FLAGS' > flags
      bin/tetherd > tetherd.log 2>&1 &
      sleep 2; echo started" ;;
  stop)   vm 'slay -f tetherd 2>/dev/null; sleep 1; for n in tether-bridge tether-ai tether-arbiter tether-hw; do slay -f $n 2>/dev/null; done; sleep 1; echo stopped' ;;
  status) vm 'pidin -F "%a %N" 2>&1 | grep tether- | sort -u; /data/tether/bin/tether-ctl hw' ;;
  logs)   vm 'cd /data/tether; for f in tetherd hw arb ai; do echo "== $f.log"; tail -${2:-8} $f.log 2>/dev/null; done' ;;
  *) echo "usage: $0 start|stop|status|logs"; exit 2 ;;
esac
