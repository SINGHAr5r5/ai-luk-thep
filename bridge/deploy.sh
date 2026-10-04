#!/bin/bash
# Install/update the bridge on the Pi from this repo folder.
# Run on the Pi:  bash ~/mac/gitlab/iot/aiBot/bridge/deploy.sh
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
DST="$HOME/voice-bridge"

mkdir -p "$DST"
rsync -a --delete --exclude venv --exclude .env --exclude '__pycache__' --exclude '*.wav' "$SRC/" "$DST/"

[ -d "$DST/venv" ] || python3 -m venv "$DST/venv"
"$DST/venv/bin/pip" install -q -r "$DST/requirements.txt"

# Device token for the board (kept only on the Pi; never commit it)
if ! grep -q '^DEVICE_TOKENS=' "$DST/.env" 2>/dev/null; then
  umask 077
  echo "DEVICE_TOKENS=$(openssl rand -hex 16)" >> "$DST/.env"
fi
chmod 600 "$DST/.env"

mkdir -p "$HOME/.config/systemd/user"
cp "$DST/systemd/voice-bridge.service" "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable voice-bridge >/dev/null
systemctl --user restart voice-bridge
echo "deployed to $DST; service: $(systemctl --user is-active voice-bridge)"
