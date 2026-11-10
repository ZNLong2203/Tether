#!/bin/bash
# Install QNX Software Center and QNX SDP 8.0 into folders on this Mac, using
# the build container (the SDP host tools are x86_64 Linux programs).
#
#   tools/docker/install-sdp.sh /path/to/qnx-setup-2.0.4-...-linux.run
#       first time: QNX Software Center, then the SDP baseline
#   tools/docker/install-sdp.sh --packages id1,id2
#       later: add packages (for example a BSP) to the SDP already installed
#
# Run it yourself in a terminal: it is interactive on purpose.
#   - the installer shows the QNX licence agreement and you answer y or n;
#   - you type your myQNX e-mail and password. The password goes into a
#     memory-only file inside the container, never to disk, to a log, or to
#     anyone else.
#
# Folders created on the Mac (override with the variables in brackets):
#   ~/qnx-qsc   QNX Software Center + install log   [QNX_QSC_HOST]
#   ~/qnx800    the SDP itself                       [QNX_SDP_HOST]
#   ~/.qnx      QNX settings and licence activation
set -euo pipefail

usage() { echo "usage: $0 /path/to/qnx-setup-*-linux.run | $0 --packages id1,id2"; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
SDP="${QNX_SDP_HOST:-$HOME/qnx800}"
MODE=full; PKGS=""; INST_DIR="$HERE"; INSTALLER=""
if [ $# -eq 2 ] && [ "$1" = "--packages" ]; then
  MODE=packages; PKGS="$2"
elif [ $# -eq 1 ] && [ -f "$1" ]; then
  INST_DIR="$(cd "$(dirname "$1")" && pwd)"; INSTALLER="$(basename "$1")"
else
  usage
fi
QSC="${QNX_QSC_HOST:-$HOME/qnx-qsc}"
ME="$(id -un)"

mkdir -p "$SDP" "$QSC" "$HOME/.qnx"
docker image inspect tether-qnx >/dev/null 2>&1 || { echo "build the image first: tools/docker/build-image.sh"; exit 1; }

exec docker run --rm -it --platform linux/amd64 \
  -v "$HERE":/scripts:ro \
  -v "$INST_DIR":/inst:ro \
  -v "$QSC":/home/"$ME"/qnx \
  -v "$SDP":/opt/qnx800 \
  -v "$HOME/.qnx":/home/"$ME"/.qnx \
  --tmpfs /secrets:rw,size=1m,mode=0700,uid="$(id -u)",gid="$(id -g)" \
  -e MODE="$MODE" -e PKGS="$PKGS" -e INSTALLER="$INSTALLER" \
  tether-qnx /scripts/install-sdp-inner.sh
