#!/usr/bin/env bash
# Remove the Unity cursor wrap plugin and everything its setup installed.
# Run this before or instead of `omarchy plugin remove`.

set -euo pipefail

plugin_id="felix.unity-cursor-wrap"
lib_dir="$HOME/.local/lib/unity-cursor-wrap"
plugin_dir="$HOME/.config/omarchy/plugins/$plugin_id"
desktop="$HOME/.local/share/applications/unityhub.desktop"

say() { printf '%s\n' "$*"; }

say "==> stopping the lifecycle watcher"
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user disable --now unity-cursor-wrap-watch.service >/dev/null 2>&1 || true
  rm -f "$HOME/.config/systemd/user/unity-cursor-wrap-watch.service"
  systemctl --user daemon-reload >/dev/null 2>&1 || true
fi

if [ -f "$desktop" ] && grep -q 'unity-cursor-wrap/unityhub' "$desktop"; then
  say "==> removing the desktop-entry override"
  rm -f "$desktop"
  command -v update-desktop-database >/dev/null 2>&1 \
    && update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
fi

say "==> removing the shell plugin"
if command -v omarchy >/dev/null 2>&1; then
  omarchy plugin remove "$plugin_id" --yes >/dev/null 2>&1 || true
fi
rm -rf "$plugin_dir"

say "==> removing the shim"
rm -rf "$lib_dir"

if command -v omarchy-shell >/dev/null 2>&1; then
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
fi

say ""
say "Removed. Restart Unity Hub and the Editor to run without the shim."
