#!/bin/bash
# Render the Devpost gallery and thumbnails from docs/devpost/src/*.html with headless Chrome.
#   tools/gallery/render.sh [name ...]      names without extension; default: all
# Sizes by file name: video-*  1280x720 (16:9, for the video thumbnail),
#                     thumb-*  1500x1000 (3:2, Devpost project thumbnail),
#                     others   1800x1200 (3:2, image gallery).
# Devpost accepts JPG, PNG or GIF up to 5 MB; 3:2 is the recommended ratio.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/docs/devpost/src"; OUT="$ROOT/docs/devpost"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
[ -x "$CHROME" ] || { echo "set CHROME to a Chrome/Chromium binary"; exit 1; }
names=("$@"); [ ${#names[@]} -gt 0 ] || { for f in "$SRC"/*.html; do names+=("$(basename "$f" .html)"); done; }
for n in "${names[@]}"; do
  case "$n" in video-*) W=1280; H=720;; thumb-*) W=1500; H=1000;; *) W=1800; H=1200;; esac
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
    --window-size=$W,$H --virtual-time-budget=4000 --screenshot="$OUT/$n.png" "file://$SRC/$n.html" >/dev/null 2>&1
  printf '%-28s %sx%s  %s KB\n' "$n.png" "$W" "$H" "$(( $(stat -f %z "$OUT/$n.png") / 1024 ))"
done
