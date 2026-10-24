import QtQuick
import Quickshell
import Quickshell.Io

// State and diagnostics for the Unity cursor-wrap shim.
//
// The plugin itself only exists while Unity runs: a small systemd user service
// (installed alongside this plugin) enables it when the Hub or an Editor
// starts and disables it completely when the last one exits.  So this service
// always starts a Unity session with wrapping enabled, and its toggle is only
// an override for the current session.
//
// CLI:
//   omarchy-shell felix.unity-cursor-wrap status
//   omarchy-shell felix.unity-cursor-wrap diagnose
//   omarchy-shell felix.unity-cursor-wrap toggle|enable|disable
Item {
  id: root

  // Injected by omarchy-shell.
  property var shell: null

  readonly property string stateDir: (Quickshell.env("XDG_STATE_HOME")
    || (Quickshell.env("HOME") + "/.local/state")) + "/omarchy"
  readonly property string statePath: root.stateDir + "/unity-cursor-wrap.json"

  readonly property string pluginDir: {
    var url = Qt.resolvedUrl("manifest.json").toString()
    return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/\/manifest\.json$/, ""))
  }

  // Read by the shim on every wrap decision.
  property bool wrapEnabled: true
  property bool stateLoaded: false

  // Live diagnostics, refreshed by status.sh.
  property int editorCount: 0
  property bool hubRunning: false
  property int shimmedCount: 0

  readonly property bool unityRunning: root.editorCount > 0
  readonly property bool needsRestart: root.editorCount > 0 && root.shimmedCount === 0

  IpcHandler {
    target: "felix.unity-cursor-wrap"

    function toggle(): string {
      return root.toggle()
    }

    function enable(): string {
      return root.enable()
    }

    function disable(): string {
      return root.disable()
    }

    function status(): string {
      return root.status()
    }

    function diagnose(): string {
      return root.diagnose()
    }

    function refresh(): string {
      root.refreshStatus()
      return root.diagnose()
    }
  }

  function toggle(): string {
    root.setEnabled(!root.wrapEnabled)
    return root.status()
  }

  function enable(): string {
    root.setEnabled(true)
    return root.status()
  }

  function disable(): string {
    root.setEnabled(false)
    return root.status()
  }

  function status(): string {
    return root.wrapEnabled ? "on" : "off"
  }

  function diagnose(): string {
    return "enabled=" + (root.wrapEnabled ? "1" : "0")
      + " editors=" + root.editorCount
      + " shimmed=" + root.shimmedCount
      + " hub=" + (root.hubRunning ? "1" : "0")
      + " unity=" + (root.unityRunning ? "1" : "0")
  }

  function setEnabled(value) {
    var next = value === true
    if (root.wrapEnabled === next) return
    root.wrapEnabled = next
    root.saveState()
  }

  function saveState() {
    if (!root.stateLoaded) return
    saveTimer.restart()
  }

  function loadState(raw) {
    if (root.stateLoaded) return
    root.stateLoaded = true
    var parsed = null
    try {
      parsed = JSON.parse(String(raw || ""))
    } catch (error) {
      parsed = null
    }
    // A fresh Unity session always starts enabled; an off state only lasts
    // until the plugin is unloaded with Unity.
    root.wrapEnabled = true
    root.saveState()
    root.refreshStatus()
  }

  function applyStatus(line) {
    var text = String(line || "")
    var fields = text.trim().split(/\s+/)
    var values = ({})
    for (var i = 0; i < fields.length; i++) {
      var parts = fields[i].split("=")
      if (parts.length === 2) values[parts[0]] = Number(parts[1])
    }
    if (values.editors !== undefined) root.editorCount = values.editors
    if (values.shimmed !== undefined) root.shimmedCount = values.shimmed
    if (values.hub !== undefined) root.hubRunning = values.hub === 1
  }

  function refreshStatus() {
    if (statusProcess.running) return
    statusProcess.running = true
  }

  FileView {
    id: stateRead
    path: root.statePath
    watchChanges: false
    printErrors: false
    onLoaded: root.loadState(text())
    onLoadFailed: root.loadState("")
  }

  FileView {
    id: stateWrite
    path: root.statePath
    watchChanges: false
    atomicWrites: true
    printErrors: false
  }

  Timer {
    id: saveTimer
    interval: 120
    repeat: false
    onTriggered: stateWrite.setText(JSON.stringify({
      version: 1,
      enabled: root.wrapEnabled
    }, null, 2) + "\n")
  }

  Timer {
    id: statusTimer
    interval: 4000
    repeat: true
    running: root.stateLoaded
    triggeredOnStart: true
    onTriggered: root.refreshStatus()
  }

  Process {
    id: mkdirProcess
    command: [ "mkdir", "-p", root.stateDir ]
    onExited: stateRead.reload()
  }

  // First load after `omarchy plugin add` (or after an update bumped the
  // version): install the shim, Hub launcher and watcher.  Detached so that
  // the watcher unloading this plugin cannot interrupt it.
  Process {
    id: setupProcess
    command: [ "bash", "-c", "setsid --fork bash \"$1\" >/dev/null 2>&1",
      "unity-cursor-wrap", root.pluginDir + "/setup.sh" ]
  }

  Process {
    id: statusProcess
    command: [ "bash", root.pluginDir + "/status.sh" ]
    stdout: SplitParser {
      onRead: function(line) {
        if (String(line || "").trim() !== "") root.applyStatus(line)
      }
    }
  }

  Component.onCompleted: {
    mkdirProcess.running = true
    setupProcess.running = true
  }
}
