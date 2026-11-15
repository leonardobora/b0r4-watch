#!/data/data/com.termux/files/usr/bin/bash
# Idempotent installer run on the Galaxy S21 once it charges & comes online.
set -euo pipefail

PKG_DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HOME/.local/bin" "$HOME/.local/state/twatch-s21" "$HOME/.termux/boot" "$HOME/.hermes/skills/twatch-s21-companion"

cp "$PKG_DIR/twatch_s21_bridge.py" "$HOME/.local/bin/twatch-s21-bridge"
cp "$PKG_DIR/twatch_s21_tui.py" "$HOME/.local/bin/twatch-s21"
cp "$PKG_DIR/SKILL.md" "$HOME/.hermes/skills/twatch-s21-companion/SKILL.md"
chmod +x "$HOME/.local/bin/twatch-s21-bridge" "$HOME/.local/bin/twatch-s21"

# Best-effort bleak install inside Termux Python if missing (non-fatal: bridge degrades gracefully)
if command -v python3 >/dev/null 2>&1; then
  python3 -c "import bleak" 2>/dev/null || pip install --quiet bleak 2>/dev/null || true
fi

# Termux:Boot auto-start script so S21 reconnects to watch after every phone reboot
cat <<'BOOT' > "$HOME/.termux/boot/20-twatch-bridge.sh"
#!/data/data/com.termux/files/usr/bin/bash
termux-wake-lock 2>/dev/null || true
pgrep -f twatch-s21-bridge >/dev/null 2>&1 || nohup "$HOME/.local/bin/twatch-s21-bridge" >"$HOME/.local/state/twatch-s21/bridge.log" 2>&1 &
BOOT
chmod +x "$HOME/.termux/boot/20-twatch-bridge.sh"

# Start/restart live bridge daemon now
pkill -f twatch-s21-bridge 2>/dev/null || true
nohup "$HOME/.local/bin/twatch-s21-bridge" >"$HOME/.local/state/twatch-s21/bridge.log" 2>&1 &
sleep 1
echo "INSTALLED_OK pid=$(pgrep -f twatch-s21-bridge | head -n 1 || echo none)"
