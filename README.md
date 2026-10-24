# Unity cursor wrap for Omarchy

Makes Unity Editor's "infinite mouse" dragging work on Omarchy. Keep dragging
at a screen edge and the pointer reappears on the opposite side, so panning,
looking around and gizmo dragging never stop.

The cause is Wayland: it does not let applications move the pointer, so the drag
stops at the edge — and a plain cursor warp would make Unity snap the camera the
other way. A small `LD_PRELOAD` shim inside the Editor wraps the pointer through
Hyprland's IPC and keeps the coordinates Unity sees continuous.

Tested with Unity 6.6.7f1 on Omarchy (Hyprland 0.56.2).

## Requirements

- Omarchy on Arch Linux
- Unity Editor installed through Unity Hub (GTK3-based, Unity 6.x)
- `gcc`, `libX11`, `libXi` — the plugin builds its shim on first load

## Install

```bash
omarchy plugin add https://github.com/bencuci/omarchy-unity-cursor-fix --enable
```

On first load the plugin installs its shim, the Unity Hub launcher and a small
watcher service, then makes itself scarce. Quit Unity Hub and start it again
**from the app menu** once, so Editors inherit the shim. Open your project and
drag into an edge.

Update later with `omarchy plugin update felix.unity-cursor-wrap`.

## Usage

- The plugin is loaded only while a Unity Editor runs: the ⇄ button appears in
  the bar and disappears when you close Unity.
- Left-click the button to override wrapping for the current session; every new
  session starts enabled.

```bash
omarchy-shell felix.unity-cursor-wrap diagnose   # is Unity running, is the shim loaded?
omarchy-shell felix.unity-cursor-wrap status     # on / off
omarchy-shell felix.unity-cursor-wrap toggle     # manual override
```

These only answer while an Editor is running (the plugin is unloaded otherwise).

## Troubleshooting

`diagnose` prints something like:

```
enabled=1 editors=1 shimmed=1 hub=1 unity=1
```

`shimmed=0` with `editors=1` means the Editor started without the shim: quit the
Hub, start it from the app menu, reopen the project.

If the first-run setup did not complete, run it by hand:

```bash
~/.config/omarchy/plugins/felix.unity-cursor-wrap/setup.sh
```

Debug log (set on the Hub's environment):

```bash
UNITY_CURSOR_WRAP_DEBUG=1
tail -f ~/.local/state/omarchy/unity-cursor-wrap.log
```

## Limitations

- The pointer visibly jumps to the opposite edge during a wrap, same as X11.
- Pan, look and gizmo drags are delta-based and unaffected; a marquee selection
  dragged past an edge can look offset.
- A Hub started from a terminal with plain `unityhub` is not covered; use the
  app menu or `~/.local/lib/unity-cursor-wrap/unityhub`.
- The Editor must be GTK3-based (Unity 6.x).

## Uninstall

```bash
~/.local/lib/unity-cursor-wrap/uninstall.sh
```

## License

MIT — see [LICENSE](LICENSE).
