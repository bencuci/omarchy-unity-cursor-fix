import QtQuick
import qs.Ui

// Bar button for the Unity cursor-wrap service.
//
// There is nothing to launch: Unity's menu entry already starts the Hub with
// the shim, and the service follows Unity's lifecycle on its own. The button
// is a status light with an optional manual override for the current session.
BarWidget {
  id: root
  moduleName: "felix.unity-cursor-wrap"

  property var service: null

  function lookupService() {
    var barObject = root.bar
    if (!barObject || !barObject.shell) return null
    if (typeof barObject.shell.serviceFor !== "function") return null
    return barObject.shell.serviceFor("felix.unity-cursor-wrap")
  }

  readonly property bool wrapOn: root.service ? root.service.wrapEnabled === true : false
  readonly property bool unityRunning: root.service ? root.service.unityRunning === true : false
  readonly property int shimmedCount: root.service ? root.service.shimmedCount : 0
  readonly property bool needsRestart: root.service ? root.service.needsRestart === true : false

  readonly property string stateText: {
    if (root.needsRestart)
      return "Unity is running without the shim — restart Unity Hub from the app menu"
    if (root.shimmedCount > 0 && root.wrapOn)
      return "Unity cursor wrap on — active in " + root.shimmedCount + " editor(s)"
    if (root.shimmedCount > 0)
      return "Unity cursor wrap off — click to enable"
    if (root.unityRunning) return "Unity cursor wrap — waiting for the Editor to open"
    return "Unity cursor wrap — idle"
  }

  function toggle() {
    if (root.service && typeof root.service.toggle === "function") {
      root.service.toggle()
      return
    }
    if (root.bar) root.bar.run("omarchy-shell felix.unity-cursor-wrap toggle")
  }

  onBarChanged: root.service = root.lookupService()

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  // Services mount after plugins are rescanned; keep polling until the lookup
  // resolves instead of binding to null forever.
  Timer {
    interval: 1000
    repeat: true
    running: root.service === null
    triggeredOnStart: true
    onTriggered: root.service = root.lookupService()
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: root.setting("icon", "⇄")
    active: root.wrapOn && root.shimmedCount > 0
    tooltipText: root.stateText
    onPressed: function(buttonPressed) {
      if (buttonPressed === Qt.LeftButton) root.toggle()
    }
  }
}
