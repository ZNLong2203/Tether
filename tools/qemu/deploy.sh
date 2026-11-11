#!/bin/bash
# Copy the cross-compiled Tether programs and the shipped models into the VM
# (running; see run-vm.sh). They land in /data/tether, which survives reboots.
#   tools/qemu/deploy.sh [build-dir]      default build-qnx
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build-qnx}"
KEY="$HOME/.qnx/tether_vm_ed25519"
KH="$ROOT/target/qemu/local/ssh-ident"
SSH=(-i "$KEY" -o IdentitiesOnly=yes -o "UserKnownHostsFile=$KH")

"$ROOT/tools/qemu/run-vm.sh" ssh 'mkdir -p /data/tether/bin /data/tether/models /data/tether/www'
scp -q "${SSH[@]}" -P 2222 "$BUILD"/qnx/tether-* root@127.0.0.1:/data/tether/bin/
scp -q "${SSH[@]}" -P 2222 "$ROOT"/models/*.tmlp root@127.0.0.1:/data/tether/models/
[ -d "$ROOT/qnx/www" ] && scp -q "${SSH[@]}" -P 2222 "$ROOT"/qnx/www/* root@127.0.0.1:/data/tether/www/ || true
"$ROOT/tools/qemu/run-vm.sh" ssh 'chmod +x /data/tether/bin/*; ls -l /data/tether/bin'
