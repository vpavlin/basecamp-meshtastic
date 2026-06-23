#!/usr/bin/env bash
# Install the headless Meshtastic gateway as a systemd --user service.
#
# Prereqs (see server/README.md):
#   - logoscore (the GUI-less Logos host) available for your arch
#   - the gateway + delivery_module + capability_module installed under a modules dir
#     (i.e. you've installed the .lgx, typically into ~/.local/share/Logos/LogosBasecamp*/modules)
#   - a Meshtastic node on USB, and your user in the 'dialout' group
#
# Override discovery via env: LOGOSCORE=... MODULES_DIR=... ./install.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

# ---- locate logoscore + modules dir -----------------------------------------
LOGOSCORE="${LOGOSCORE:-$(command -v logoscore || true)}"
[[ -n "$LOGOSCORE" ]] || LOGOSCORE="$HOME/.local/bin/logoscore"
MODULES_DIR="${MODULES_DIR:-}"
if [[ -z "$MODULES_DIR" ]]; then
  for d in "$HOME"/.local/share/Logos/LogosBasecamp*/modules; do
    [[ -d "$d/meshtastic_gateway" ]] && MODULES_DIR="$d" && break
  done
fi

echo "logoscore   : $LOGOSCORE"
echo "modules dir : ${MODULES_DIR:-<not found>}"
[[ -x "$LOGOSCORE" ]] || { echo "ERROR: logoscore not executable; set LOGOSCORE=..." >&2; exit 1; }
[[ -n "$MODULES_DIR" && -d "$MODULES_DIR/meshtastic_gateway" ]] || {
  echo "ERROR: meshtastic_gateway not found under a modules dir; set MODULES_DIR=..." >&2; exit 1; }
for m in capability_module delivery_module; do
  [[ -d "$MODULES_DIR/$m" ]] || echo "WARN: dependency '$m' not found in $MODULES_DIR — install its .lgx" >&2
done

# ---- install scripts + env + unit -------------------------------------------
mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"
install -m 0755 "$here/meshtastic-gateway-run" "$HOME/.local/bin/meshtastic-gateway-run"
install -m 0755 "$here/gwctl"                  "$HOME/.local/bin/gwctl"

cat > "$HOME/.config/meshtastic-gateway.env" <<EOF
LOGOSCORE=$LOGOSCORE
MODULES_DIR=$MODULES_DIR
EOF

install -m 0644 "$here/meshtastic-gateway.service" "$HOME/.config/systemd/user/meshtastic-gateway.service"

# ---- enable + start ----------------------------------------------------------
systemctl --user daemon-reload
systemctl --user enable meshtastic-gateway.service
# run without an active login session (servers/Pi): persist the user manager
loginctl enable-linger "$USER" 2>/dev/null || \
  echo "NOTE: could not enable-linger; run 'sudo loginctl enable-linger $USER' so it survives logout"

echo
echo "Installed. Next:"
echo "  1. Configure which channels to relay (daemon must be stopped):"
echo "       systemctl --user stop meshtastic-gateway"
echo "       gwctl channels                 # see indices"
echo "       gwctl relay 1 on               # bridge channel 1, etc."
echo "  2. Start it:"
echo "       systemctl --user start meshtastic-gateway"
echo "       journalctl --user -u meshtastic-gateway -f"
