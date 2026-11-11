#!/bin/bash
# Cross-compile the Tether processes for QNX 8.0 (in the build container).
#   tools/build-qnx.sh                 aarch64le, output in build-qnx/
#   QNX_ARCH=x86_64 tools/build-qnx.sh x86_64, output in build-qnx-x86_64/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${QNX_ARCH:-aarch64le}"
OUT="build-qnx"; [ "$ARCH" = aarch64le ] || OUT="build-qnx-$ARCH"
exec "$ROOT/tools/docker/run.sh" bash -lc "
  set -euo pipefail
  source /opt/qnx800/qnxsdp-env.sh >/dev/null 2>&1
  cmake -S /work -B /work/$OUT -G Ninja -DCMAKE_TOOLCHAIN_FILE=/work/cmake/qnx800.cmake -DQNX_ARCH=$ARCH >/dev/null
  cmake --build /work/$OUT
"
