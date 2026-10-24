#!/usr/bin/env bash
# Install the machine-side pieces of the Unity cursor wrap plugin: the
# LD_PRELOAD shim, the Unity Hub launcher, the desktop-entry override and the
# systemd watcher.  Service.qml runs this when the plugin loads; it is
# idempotent and exits immediately once the installed version matches.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib_dir="$HOME/.local/lib/unity-cursor-wrap"
marker="$lib_dir/installed"

version="1"
if command -v jq >/dev/null 2>&1 && [ -f "$here/manifest.json" ]; then
  version="$(jq -r '.version // "1"' "$here/manifest.json")"
fi

if [ -f "$marker" ] && [ "$(cat "$marker" 2>/dev/null)" = "$version" ] \
   && [ -f "$lib_dir/libunity-cursor-wrap.so" ] \
   && [ -x "$lib_dir/unityhub" ] \
   && [ -x "$lib_dir/watch.sh" ]; then
  exit 0
fi

say() { printf '%s\n' "$*"; }

say "unity-cursor-wrap: building the shim"
make -C "$here/shim"

install -d "$lib_dir"
install -m 0755 "$here/shim/libunity-cursor-wrap.so" "$lib_dir/libunity-cursor-wrap.so"
install -m 0755 "$here/launcher/unityhub" "$lib_dir/unityhub"
install -m 0755 "$here/watcher/unity-cursor-wrap-watch.sh" "$lib_dir/watch.sh"
install -m 0755 "$here/uninstall.sh" "$lib_dir/uninstall.sh"

system_desktop="/usr/share/applications/unityhub.desktop"
if [ -f "$system_desktop" ]; then
  install -d "$HOME/.local/share/applications"
  python3 - "$system_desktop" \
    "$HOME/.local/share/applications/unityhub.desktop" "$lib_dir/unityhub" <<'PY'
import sys

source, target, launcher = sys.argv[1:4]

with open(source, encoding="utf-8") as fh:
    lines = fh.readlines()

exec_line = 'Exec="%s" %%U\n' % launcher

out = []
replaced = False
for line in lines:
    if line.startswith("Exec="):
        out.append(exec_line)
        replaced = True
    else:
        out.append(line)

if not replaced:
    out.append(exec_line)

with open(target, "w", encoding="utf-8") as fh:
    fh.writelines(out)
PY
  command -v update-desktop-database >/dev/null 2>&1 \
    && update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
fi

install -d "$HOME/.config/systemd/user"
install -m 0644 "$here/watcher/unity-cursor-wrap-watch.service" \
  "$HOME/.config/systemd/user/unity-cursor-wrap-watch.service"

if command -v systemctl >/dev/null 2>&1; then
  systemctl --user daemon-reload >/dev/null 2>&1 || true
  systemctl --user enable --now unity-cursor-wrap-watch.service >/dev/null 2>&1 || true
fi

printf '%s\n' "$version" > "$marker"
say "unity-cursor-wrap: ready"
