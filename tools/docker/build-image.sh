#!/bin/bash
# Build the tether-qnx build image (linux/amd64, emulated on Apple Silicon).
set -euo pipefail
cd "$(dirname "$0")"
docker build --platform linux/amd64 -t tether-qnx \
  --build-arg USER_NAME="$(id -un)" --build-arg USER_ID="$(id -u)" --build-arg GROUP_ID="$(id -g)" .
