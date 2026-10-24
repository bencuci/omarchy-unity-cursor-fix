#!/usr/bin/env bash
# Enable the Omarchy shell plugin only while a Unity Editor is running.
#
# The plugin is unloaded completely (widget removed from the bar, service torn
# down) when no Editor process exists, and re-enabled as soon as one starts.
# Unity Hub alone does not count: it is only a launcher.  This runs as a
# systemd user service.

set -u

plugin="felix.unity-cursor-wrap"
omarchy_bin="/usr/share/omarchy/bin/omarchy"
last=""

unity_running() {
  for exe in /proc/[0-9]*/exe; do
    target="$(readlink "$exe" 2>/dev/null)" || continue
    case "${target##*/}" in
      unityhub-unity-editor*|Unity)
        # Asset import workers share the Editor binary but are headless.
        pid="${exe#/proc/}"
        pid="${pid%/exe}"
        cmdline="$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null)"
        case "$cmdline" in
          *-batchMode*) ;;
          *) return 0 ;;
        esac
        ;;
    esac
  done
  return 1
}

while :; do
  if unity_running; then
    want="on"
  else
    want="off"
  fi

  if [ "$want" != "$last" ]; then
    if [ "$want" = "on" ]; then
      if "$omarchy_bin" plugin enable "$plugin" --section right >/dev/null 2>&1; then
        last="$want"
      fi
    else
      if "$omarchy_bin" plugin disable "$plugin" >/dev/null 2>&1; then
        last="$want"
      fi
    fi
  fi

  sleep 3
done
