#!/usr/bin/env bash
# Report the state of the Unity cursor-wrap shim.
#
#   editors=<n>            Unity Editor processes
#   shimmed=<n>            ... of those, how many have the shim loaded
#   hub=<0|1>              is Unity Hub running
#   hub_shimmed=<0|1>      was the Hub started with the shim (so new editors get it)
#
# Deliberately scans /proc/*/exe instead of using pgrep -f: the pattern of a
# pgrep command would match the command line of the shell running it.

shim_name="libunity-cursor-wrap.so"
editors=0
shimmed=0
hub=0
hub_shimmed=0

is_editor() {
  case "${1##*/}" in
    unityhub-unity-editor*) return 0 ;;
    Unity) return 0 ;;
    *) return 1 ;;
  esac
}

for exe in /proc/[0-9]*/exe; do
  target="$(readlink "$exe" 2>/dev/null)" || continue
  pid="${exe#/proc/}"
  pid="${pid%/exe}"

  if is_editor "$target"; then
    # Asset import workers are the same binary in batch mode; not an editor.
    cmdline="$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null)"
    case "$cmdline" in
      *-batchMode*) continue ;;
    esac
    editors=$((editors + 1))
    if grep -qs "$shim_name" "/proc/$pid/maps" 2>/dev/null; then
      shimmed=$((shimmed + 1))
    fi
    continue
  fi

  if [ "${target##*/}" = "unityhub-bin" ]; then
    hub=1
    if grep -qs "$shim_name" "/proc/$pid/maps" 2>/dev/null; then
      hub_shimmed=1
    fi
  fi
done

printf 'editors=%d shimmed=%d hub=%d hub_shimmed=%d\n' \
  "$editors" "$shimmed" "$hub" "$hub_shimmed"
