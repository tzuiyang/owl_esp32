#!/usr/bin/env bash
# deploy-ui.sh — push web-src/ to the device over HTTP.
#
# Usage:
#   ./deploy-ui.sh                       # uses owl.local
#   ./deploy-ui.sh 192.168.4.1           # explicit host (if mDNS isn't resolving)
#
# Requires: Mac joined to the device's "owl" WiFi network. The device's
# firmware must include the POST /web/<filename> endpoint (added in
# feature/ui-improve onward).
#
# Each file in web-src/ is sent as a raw POST body to /web/<filename>.
# The firmware writes it to the SD card's /web/ folder. After this
# completes, refresh http://owl.local/ to see your changes — no card
# reader, no SD popping.

set -e

SRC="$(cd "$(dirname "$0")" && pwd)/web-src"
HOST="${1:-owl.local}"

if [ ! -d "$SRC" ]; then
  echo "error: $SRC not found"
  exit 1
fi

# Map file extension to a sensible Content-Type for the firmware.
mime_for() {
  case "$1" in
    *.html) echo "text/html" ;;
    *.css)  echo "text/css" ;;
    *.js)   echo "application/javascript" ;;
    *.json) echo "application/json" ;;
    *.png)  echo "image/png" ;;
    *.svg)  echo "image/svg+xml" ;;
    *.ico)  echo "image/x-icon" ;;
    *.woff2) echo "font/woff2" ;;
    *)      echo "application/octet-stream" ;;
  esac
}

echo "uploading $SRC/* -> http://$HOST/web/"

count=0
for f in "$SRC"/*; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  size=$(wc -c < "$f" | tr -d ' ')
  mime=$(mime_for "$f")
  printf "  %-22s %6s bytes  " "$name" "$size"
  if curl -fsS -X POST \
       --max-time 30 \
       --data-binary "@$f" \
       -H "Content-Type: $mime" \
       "http://$HOST/web/$name" >/dev/null; then
    echo "ok"
    count=$((count+1))
  else
    echo "FAILED"
    echo "(make sure your Mac is joined to the 'owl' WiFi and the device firmware has the /web/ POST handler)"
    exit 1
  fi
done

echo "done. $count file(s) uploaded. Refresh http://$HOST/ to see changes."
