#!/usr/bin/env bash
# Push a firmware image to the board over Wi-Fi (no cable).
#   tools/ota_push.sh [firmware.bin]        default: .pio/build/usb/firmware.bin
# Config (env or firmware/.env, never committed):
#   OTA_TOKEN (required)  OTA_HOST (default ai-luk-thep.local)  OTA_IP (fallback when mDNS is slow)
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f .env ] && { set -a; . ./.env; set +a; }
: "${OTA_TOKEN:?OTA_TOKEN missing - read it with the token console command and put it in firmware/.env}"
HOST="${OTA_HOST:-ai-luk-thep.local}"
# macOS can take several seconds to resolve .local names; fall back to a fixed IP if given
if ! curl -fsS --max-time 12 "http://$HOST/info" >/dev/null 2>&1 && [ -n "${OTA_IP:-}" ]; then
  echo "note: $HOST not resolving, using $OTA_IP"; HOST="$OTA_IP"
fi
BIN="${1:-.pio/build/usb/firmware.bin}"
[ -f "$BIN" ] || { echo "no such file: $BIN" >&2; exit 1; }
SHA=$(shasum -a 256 "$BIN" | cut -d' ' -f1)

before=$(curl -fsS --max-time 12 "http://$HOST/info" || true)
echo "board now : ${before:-unreachable}"
echo "pushing   : $BIN ($(wc -c < "$BIN" | tr -d ' ') bytes, sha256 ${SHA:0:12}...)"
curl -fsS --max-time 180 -X POST --data-binary @"$BIN" \
     -H "X-OTA-Token: $OTA_TOKEN" -H "X-OTA-SHA256: $SHA" \
     -H "Content-Type: application/octet-stream" "http://$HOST/ota"
echo
echo "waiting for the board to reboot..."
sleep 6
for i in $(seq 1 30); do
  after=$(curl -fsS --max-time 8 "http://$HOST/info" 2>/dev/null || true)
  if [ -n "$after" ]; then echo "board now : $after"; exit 0; fi
  sleep 3
done
echo "board did not come back within 90 s" >&2
exit 1
