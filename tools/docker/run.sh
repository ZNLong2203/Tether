#!/bin/bash
# Run a command (default: a shell) in the build container.
#   tools/docker/run.sh                      interactive shell
#   tools/docker/run.sh qnx-build.sh         anything on PATH inside the container
# Mounts only: this repository (read-write), the QNX SDP (read-only), and the
# QNX licence folder. No --privileged, no host networking, no access to $HOME.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDP="${QNX_SDP_HOST:-$HOME/qnx800}"
[ -d "$SDP" ] || { echo "QNX SDP not found at $SDP (set QNX_SDP_HOST)"; exit 1; }
mkdir -p "$HOME/.qnx"
# -t only when a terminal is attached, so the script also works from CI and tools.
TTY=(); [ -t 0 ] && TTY=(-it)
[ $# -gt 0 ] || set -- /bin/bash -l
exec docker run --rm ${TTY[@]+"${TTY[@]}"} --platform linux/amd64 \
  -v "$REPO":/work \
  -v "$SDP":/opt/qnx800:ro \
  -v "$HOME/.qnx":/home/"$(id -un)"/.qnx \
  -w /work \
  tether-qnx "$@"
