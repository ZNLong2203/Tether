#!/bin/bash
# Build the QNX 8.0 aarch64le QEMU image into target/qemu/output.
#   tools/qemu/build-image.sh            build (or rebuild after a change)
#   tools/qemu/build-image.sh --clean    also throw away cached intermediate files
#
# mkqnximage runs inside the build container (tools/docker/run.sh). Our own
# inputs are in qnx/image/ (snippets, default flags) and are copied into
# mkqnximage's local/ folder first. The programs are cross-compiled first and
# baked into the data partition; tetherd starts at boot. target/ is generated and git-ignored: it contains private keys.
#
# Needs, from the QNX Software Center (tools/docker/install-sdp.sh --packages):
#   com.qnx.qnx800.target.qemuvirt and the virtio driver packages.
# SSH: root logs in with the key ~/.qnx/tether_vm_ed25519 (made once by hand:
#   ssh-keygen -t ed25519 -N '' -C tether-vm -f ~/.qnx/tether_vm_ed25519).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$HOME/.qnx/tether_vm_ed25519.pub" ] || { echo "missing ~/.qnx/tether_vm_ed25519.pub (see header of this script)"; exit 1; }

"$ROOT/tools/build-qnx.sh" # the image embeds the programs

exec "$ROOT/tools/docker/run.sh" bash -lc '
  set -euo pipefail
  source /opt/qnx800/qnxsdp-env.sh >/dev/null 2>&1
  mkdir -p /work/target/qemu/local/snippets
  cp /work/qnx/image/snippets/* /work/target/qemu/local/snippets/
  cd /work/target/qemu
  mkqnximage --type=qemu --arch=aarch64le --noprompt \
             --ssh-ident="$HOME/.qnx/tether_vm_ed25519.pub" --build "$@"
' _ "$@"
