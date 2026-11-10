#!/bin/bash
# Run the QNX image on the Mac's own QEMU (brew install qemu).
#   tools/qemu/run-vm.sh            foreground, serial console on this terminal (quit: ctrl-a x)
#   tools/qemu/run-vm.sh start      background; console log in target/qemu/serial.log
#   tools/qemu/run-vm.sh stop
#   tools/qemu/run-vm.sh ssh [cmd]  root shell (or one command) over the forwarded port
#
# Acceleration: this is TCG (software emulation) on purpose. QEMU's HVF
# accelerator on Apple Silicon stops with "Assertion failed: (isv)" while the QNX
# kernel starts, so latency numbers taken here show the scheduler and the code
# paths, not the speed of real hardware.
#
# Forwarded to localhost: 2222 -> ssh (22), 8080 -> the Tether dashboard (8080).
# The machine type is virt-8.2: mkqnximage asks for virt-4.2, which QEMU 11 removed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/target/qemu"
KEY="$HOME/.qnx/tether_vm_ed25519"
[ -f "$W/output/ifs.bin" ] || { echo "no image: run tools/qemu/build-image.sh first"; exit 1; }
MAC="$(sed -n "s/^OPT_MACADDR='\(.*\)'/\1/p" "$W/output/options" | head -1)"

qemu_args=(
  -machine virt-8.2 -accel tcg -cpu cortex-a57 -smp 2 -m 1G
  -drive "file=$W/output/disk-qemu.vmdk,if=none,id=drv0" -device virtio-blk-device,drive=drv0
  -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:8080"
  -device "virtio-net-device,netdev=net0,mac=${MAC:-52:54:00:a3:a8:db}"
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0
  -kernel "$W/output/ifs.bin"
)

case "${1:-fg}" in
  fg)    exec qemu-system-aarch64 "${qemu_args[@]}" -nographic ;;
  start) rm -f "$W/serial.log"
         qemu-system-aarch64 "${qemu_args[@]}" -display none -monitor none \
           -serial "file:$W/serial.log" -daemonize -pidfile "$W/qemu.pid"
         echo "started, pid $(cat "$W/qemu.pid"); log: $W/serial.log" ;;
  stop)  [ -f "$W/qemu.pid" ] && kill "$(cat "$W/qemu.pid")" && rm -f "$W/qemu.pid" && echo stopped || echo "not running" ;;
  ssh)   shift
         exec ssh -i "$KEY" -p 2222 -o IdentitiesOnly=yes -o "UserKnownHostsFile=$W/local/ssh-ident" \
              root@127.0.0.1 "$@" ;;
  *)     echo "usage: $0 [start|stop|ssh [cmd]]"; exit 2 ;;
esac
