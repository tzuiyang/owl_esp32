#!/usr/bin/env bash
# deploy-ui.sh — copy web-src/ to the SD card mounted on this Mac.
#
# Usage:
#   ./deploy-ui.sh                            # auto-find the card under /Volumes
#   ./deploy-ui.sh /Volumes/MYCARDNAME        # explicit path
#
# Pop the device's microSD into your Mac's card reader first.
# After this completes, eject the card, put it back in the device, and
# refresh http://owl.local/ to see your changes — no reflash needed.

set -e

SRC="$(cd "$(dirname "$0")" && pwd)/web-src"

if [ -n "$1" ]; then
  CARD="$1"
else
  # Default: pick the first removable volume that isn't the boot drive.
  CARD=$(ls -d /Volumes/* 2>/dev/null \
    | grep -v "^/Volumes/Macintosh HD$" \
    | grep -v "^/Volumes/com.apple" \
    | head -1)
fi

if [ -z "$CARD" ] || [ ! -d "$CARD" ]; then
  echo "error: no SD card volume found. Insert the card and retry, or pass /Volumes/<name> as an argument."
  exit 1
fi

DST="$CARD/web"
echo "deploying $SRC/* -> $DST/"
mkdir -p "$DST"
cp -v "$SRC"/* "$DST/"
echo "done. Eject the card, put it back in the device, refresh http://owl.local/."
