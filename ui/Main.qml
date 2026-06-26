import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Meshtastic Gateway — desktop chat-style front-end for the meshtastic_gateway core module.
// Left sidebar lists the connected node's channels (encryption badge, relay state, unread count);
// right pane is the per-channel chat (sender names, delivery/ack status, emoji reactions, reply).
// Self-contained styling: ui_qml runs sandboxed, so we reproduce the Logos palette here.
Item {
    id: root

    // ── node / channel state ──────────────────────────────────────
    property bool nodePresent: false
    property string linkState: "searching"     // searching | connecting | connected (LoRa node)
    property string deliveryState: "down"      // down | connecting | ready (Logos Messaging)
    property string nodeName: ""
    property int relayingCount: 0
    property int nodesTotal: 0
    property int nodesOnline: 0

    // ── chat selection ────────────────────────────────────────────
    property int openChannel: -1            // selected channelIndex, -1 = none
    property string selName: ""
    property string selPsk: ""              // pskStatus: none | default | custom
    property bool selRelaying: false
    property string selTopic: ""
    property string selShareUrl: ""
    property string selRole: ""
    property bool shareOpen: false          // share-channel modal
    property int qrN: 0                      // QR matrix size (from the `qr` module), 0 = none
    property var qrCells: []                 // QR cells (row-major booleans)
    property bool createChanOpen: false     // create-channel modal
    property bool confirmDelOpen: false     // delete-channel confirm modal
    property var msgList: []                // JS array of message objects for the open channel
    property string statusMsg: ""           // transient "publishing…" banner text

    // ── view nav + nodes ──────────────────────────────────────────
    property string view: "chat"            // "chat" | "nodes"
    property var nodeList: []               // JS array of node objects (from nodesChanged)
    property real selNodeNum: -1            // selected node 'num' (real: node nums are uint32 > int max)
    onSelNodeNumChanged: mapCanvas.requestPaint()   // redraw the map's selection highlight
    property real mapZoom: 1.0              // map zoom: 1 = fit all nodes; >1 = zoomed in
    onMapZoomChanged: mapCanvas.requestPaint()

    // ── settings (persisted backend-side; see settingsChanged) ────
    property var settings: ({ onlineWindowSec: 7200, maxMsgsPerChannel: 0, distanceUnit: "km" })
    property bool settingsOpen: false
    property bool ownerSaved: false      // transient "✓ Saved" flash in the settings modal

    readonly property var reactChoices: ["👍", "❤️", "😂", "🔥", "‼️"]

    readonly property var t: ({
        bg:        "#171717", bgSec: "#262626", bgElev: "#0E121B",
        text:      "#FFFFFF", textSec: "#A4A4A4", textMuted: "#5C5C5C",
        border:    "#2B303B", primary: "#ED7B58", primaryHi: "#F55702",
        success:   "#6CCC93", mesh: "#6CCC93", logos: "#ED7B58",
        warn:      "#E0C341", danger: "#E5484D", onPrimary: "#1A0F0A",
        sm: 8, md: 12, lg: 16, radius: 8, fTitle: 18, fBody: 14, fSmall: 12, fMono: 11
    })

    // ── gateway bridge (signal-driven; NEVER pull data synchronously) ──────────
    // logos.callModule() is SYNCHRONOUS — it spins a nested event loop on the UI thread until the
    // C++ replies, and an incoming event during that loop re-enters here → unbounded nesting → freeze.
    // So gw() is FIRE-AND-FORGET: we only ever *trigger* actions; the return value is ignored. All
    // data arrives via events (nodeStatus / channelsChanged / messagesChanged) carrying full JSON
    // payloads, handled in the Connections block below. See DATAFLOWS.md.
    function gw(method, args) {
        if (typeof logos === "undefined" || !logos.callModule) return
        // Defer so a click handler / event handler returns before the IPC round-trip starts.
        Qt.callLater(function () {
            try { logos.callModule("meshtastic_gateway", method, args || []) } catch (e) {}
        })
    }

    // ── apply pushed state (called from onModuleEventReceived) ─────────────────
    function applyStatus(json) {
        try {
            var st = JSON.parse(json)
            root.nodePresent = !!st.nodePresent
            root.linkState = st.linkState || (st.nodePresent ? "connected" : "searching")
            root.deliveryState = st.deliveryState || "down"
            root.nodeName = st.nodeName || ""
            root.nodesTotal = st.nodesTotal || 0
            root.nodesOnline = st.nodesOnline || 0
        } catch (e) { /* ignore malformed */ }
    }
    function applyChannels(json) {
        try {
            var chans = JSON.parse(json)
            if (!Array.isArray(chans)) return
            channels.clear()
            var rc = 0
            for (var i = 0; i < chans.length; i++) {
                channels.append(chans[i])
                if (chans[i].relaying) rc++
            }
            root.relayingCount = rc
            syncSelected()
        } catch (e) { /* ignore malformed */ }
    }
    function applyMessages(channelIndex, json) {
        if (channelIndex !== root.openChannel) return   // event for a channel we're not viewing
        try {
            var parsed = JSON.parse(json)
            if (Array.isArray(parsed)) {
                root.msgList = parsed
                Qt.callLater(function () { chatView.positionViewAtEnd() })
            }
        } catch (e) { /* keep current list */ }
    }
    function applyNodes(json) {
        try {
            var ns = JSON.parse(json)
            if (!Array.isArray(ns)) return
            // sort: self first, then online, then most-recently-heard, then name
            ns.sort(function (a, b) {
                if (!!a.isSelf !== !!b.isSelf) return a.isSelf ? -1 : 1
                if (!!a.online !== !!b.online) return a.online ? -1 : 1
                var la = (a.lastHeardAgo === undefined || a.lastHeardAgo < 0) ? 1e12 : a.lastHeardAgo
                var lb = (b.lastHeardAgo === undefined || b.lastHeardAgo < 0) ? 1e12 : b.lastHeardAgo
                if (la !== lb) return la - lb
                return (a.name || "").localeCompare(b.name || "")
            })
            root.nodeList = ns
            mapCanvas.requestPaint()       // map is static — repaint on data change
        } catch (e) { /* keep current list */ }
    }
    function selectedNode() {
        for (var i = 0; i < root.nodeList.length; i++)
            if (root.nodeList[i].num === root.selNodeNum) return root.nodeList[i]
        return null
    }
    function applySettings(json) {
        try { var s = JSON.parse(json); if (s && typeof s === "object") root.settings = s }
        catch (e) { /* keep current */ }
        mapCanvas.requestPaint()           // distance units may have changed
    }
    function setSetting(key, value) { gw("setSetting", [key, String(value)]) }

    function selfNode() {
        for (var i = 0; i < root.nodeList.length; i++)
            if (root.nodeList[i].isSelf) return root.nodeList[i]
        return null
    }
    function selfLongName() { var n = selfNode(); return n && n.longName ? n.longName : root.nodeName }
    function selfShortName() { var n = selfNode(); return n && n.name ? n.name : "" }
    function setOwner(longName, shortName) { gw("setOwner", [longName, shortName]) }
    function zoomMap(f) { root.mapZoom = Math.max(0.25, Math.min(50, root.mapZoom * f)) }
    function resetZoom() { root.mapZoom = 1.0 }

    function syncSelected() {
        if (root.openChannel < 0) return
        for (var i = 0; i < channels.count; i++) {
            var c = channels.get(i)
            if (c.channelIndex === root.openChannel) {
                root.selName = c.displayName; root.selPsk = c.pskStatus
                root.selRelaying = c.relaying; root.selTopic = c.topic
                root.selShareUrl = c.shareUrl || ""; root.selRole = c.role || ""
                return
            }
        }
    }

    function selectChannel(idx) {
        root.openChannel = idx
        root.msgList = []          // clear immediately; the messagesChanged event repopulates
        syncSelected()
        gw("markRead", [idx])      // clears unread badge (fire-and-forget)
        loadMessages()
    }
    function closeChat() { root.openChannel = -1 }

    // Ask the backend to PUSH this channel's history; applyMessages() renders it from the
    // messagesChanged event. No synchronous pull, so no nested-event-loop freeze.
    function loadMessages() {
        if (root.openChannel < 0) return
        gw("requestMessages", [root.openChannel])
    }

    function sendCurrent() {
        var text = input.text
        if (!text || !text.trim()) return
        input.text = ""                       // clear immediately so it feels instant

        // Optimistic bubble: show the message right away in a "queued" state. The real one (with a
        // server id + cycling ack) replaces it when the messagesChanged event reloads the list.
        var arr = (root.msgList || []).slice()
        arr.push({ id: -1, from: "me", text: text, outgoing: true, origin: "local",
                   relayed: root.selRelaying, ackStatus: "queued", reactions: [] })
        root.msgList = arr
        Qt.callLater(function () { chatView.positionViewAtEnd() })

        // explicit "what's happening" banner
        flashStatus(root.selRelaying
                    ? "⇄ Publishing to Logos Messaging…"
                    : "Sending on mesh…")

        // Fire-and-forget; gw() defers the IPC so the optimistic bubble paints first. The real
        // message (server id + ack) replaces it when the messagesChanged event reloads the list.
        gw("sendMessage", [root.openChannel, text])
    }

    function flashStatus(msg) { root.statusMsg = msg; statusTimer.restart() }

    function react(messageId, emoji) {
        try { gw("addReaction", [root.openChannel, messageId, emoji]) } catch (e) {}
    }

    function toggleRelay(idx, enabled) {
        try { gw("setRelay", [idx, enabled]) } catch (e) {}
    }
    function doCreateChannel(name) {
        if (name && name.trim()) { gw("createChannel", [name.trim(), ""]); root.createChanOpen = false }
    }
    function doImportChannel(url) {
        if (url && url.indexOf("#") >= 0) { gw("addChannelFromUrl", [url.trim()]); root.createChanOpen = false }
    }
    function doDeleteChannel() {
        if (root.openChannel > 0) { gw("deleteChannel", [root.openChannel]); root.openChannel = -1 }
        root.confirmDelOpen = false
    }
    // Ask the `qr` core module for the QR matrix of `text` (pure local compute → fast sync call OK).
    function genQr(text) {
        root.qrN = 0; root.qrCells = []
        if (!text || typeof logos === "undefined" || !logos.callModule) return
        try {
            var raw = logos.callModule("qr", "generate", [text])
            var v = JSON.parse(raw); if (typeof v === "string") v = JSON.parse(v)   // callModule double-encodes
            if (v && v.ok) { root.qrN = v.n; root.qrCells = v.cells }
        } catch (e) { /* qr module unavailable — UI shows a fallback line */ }
    }

    // ── small view helpers ────────────────────────────────────────
    function ackGlyph(s) { return s === "delivered" ? "✓✓" : s === "enroute" ? "◌"
                                : s === "queued" ? "◌" : s === "failed" ? "✕" : "" }
    function ackColor(s) { return s === "delivered" ? t.success : s === "failed" ? t.danger : t.textSec }
    function ackLabel(s) { return s === "delivered" ? "delivered" : s === "enroute" ? "sending…"
                                : s === "queued" ? "queued" : s === "failed" ? "failed" : "" }
    function pskColor(s) { return s === "custom" ? t.success : s === "default" ? t.warn : t.danger }
    function pskLabel(s) { return s === "custom" ? "custom PSK" : s === "default" ? "default PSK" : "no PSK" }

    // node formatting helpers
    function fmtAgo(s) {
        if (s === undefined || s < 0) return "never"
        if (s < 60) return Math.round(s) + "s ago"
        if (s < 3600) return Math.round(s / 60) + "m ago"
        if (s < 86400) return Math.round(s / 3600) + "h ago"
        return Math.round(s / 86400) + "d ago"
    }
    function fmtDist(m) {
        if (m === undefined) return ""
        if (root.settings.distanceUnit === "mi") {
            var mi = m / 1609.344
            return mi < 0.1 ? Math.round(m * 3.28084) + " ft" : mi.toFixed(mi < 10 ? 1 : 0) + " mi"
        }
        return m < 1000 ? Math.round(m) + " m" : (m / 1000).toFixed(m < 10000 ? 1 : 0) + " km"
    }
    function fmtCompass(deg) {
        if (deg === undefined) return ""
        var dirs = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
        return dirs[Math.round(deg / 45) % 8] + " " + Math.round(deg) + "°"
    }
    function battColor(b) {
        if (b === undefined) return t.textMuted
        return b > 100 ? t.logos : b >= 40 ? t.success : b >= 15 ? t.warn : t.danger
    }
    function battLabel(b) { return b === undefined ? "—" : (b > 100 ? "PWR" : b + "%") }
    function snrColor(s) { return s === undefined ? t.textMuted : s >= 0 ? t.success : s >= -10 ? t.warn : t.danger }

    // build the label/value rows for a node's detail pane
    function nodeStatRows(n) {
        if (!n) return []
        var r = []
        r.push({ label: "Status", value: n.isSelf ? "this node" : (n.online ? "online" : "offline"),
                 color: n.isSelf ? t.logos : (n.online ? t.success : t.textMuted) })
        if (!n.isSelf) r.push({ label: "Last heard", value: fmtAgo(n.lastHeardAgo) })
        if (n.role !== undefined) r.push({ label: "Role", value: n.role })
        if (n.hwModel !== undefined) r.push({ label: "Hardware", value: n.hwModel })
        if (n.hopsAway !== undefined && !n.isSelf)
            r.push({ label: "Hops away", value: n.hopsAway + (n.hopsAway === 0 ? " (direct)" : "") })
        if (n.snr !== undefined && !n.isSelf)
            r.push({ label: "SNR", value: n.snr.toFixed(2) + " dB", color: snrColor(n.snr) })
        if (n.distanceM !== undefined) r.push({ label: "Distance", value: fmtDist(n.distanceM) })
        if (n.bearingDeg !== undefined) r.push({ label: "Bearing", value: fmtCompass(n.bearingDeg) })
        if (n.battery !== undefined)
            r.push({ label: "Battery", value: battLabel(n.battery)
                     + (n.voltage !== undefined ? " · " + n.voltage.toFixed(2) + " V" : ""),
                     color: battColor(n.battery) })
        if (n.chUtil !== undefined) r.push({ label: "Channel util", value: n.chUtil.toFixed(1) + " %" })
        if (n.airUtil !== undefined) r.push({ label: "Air util (TX)", value: n.airUtil.toFixed(1) + " %" })
        if (n.hasPos) r.push({ label: "Position", value: n.lat.toFixed(5) + ", " + n.lon.toFixed(5)
                               + (n.alt !== undefined ? " · " + n.alt + " m" : "") })
        if (n.viaMqtt) r.push({ label: "Heard via", value: "MQTT" })
        r.push({ label: "Node ID", value: "!" + (n.num >>> 0).toString(16) })
        return r
    }

    // per-message relay/origin tag (distinct from the mesh delivery ack)
    function relayText(msg) {
        if (msg.origin === "lm") return "⇄ from Logos Messaging"
        if (msg.relayed) return "⇄ relayed to Logos Messaging"
        return ""
    }
    function relayColor(msg) { return msg.origin === "lm" ? t.logos : t.success }

    // ── events ────────────────────────────────────────────────────
    // Subscribe to the backend's push events, then ask for an initial snapshot (fire-and-forget).
    // No polling Timer, no synchronous data pulls — everything below renders from events.
    Component.onCompleted: {
        if (typeof logos !== "undefined" && logos.onModuleEvent) {
            logos.onModuleEvent("meshtastic_gateway", "nodeStatus")
            logos.onModuleEvent("meshtastic_gateway", "channelsChanged")
            logos.onModuleEvent("meshtastic_gateway", "messagesChanged")
            logos.onModuleEvent("meshtastic_gateway", "nodesChanged")
            logos.onModuleEvent("meshtastic_gateway", "settingsChanged")
            logos.onModuleEvent("meshtastic_gateway", "ownerSaved")
        }
        gw("requestSnapshot")
    }
    Timer { id: statusTimer; interval: 2600; onTriggered: root.statusMsg = "" }
    Timer { id: ownerSavedTimer; interval: 2600; onTriggered: root.ownerSaved = false }
    Connections {
        target: typeof logos !== "undefined" ? logos : null
        ignoreUnknownSignals: true
        // The one inbound data path. Each event carries its full state in the payload:
        //   nodeStatus      -> d[0] = status JSON
        //   channelsChanged -> d[0] = channels JSON
        //   messagesChanged -> d[0] = channelIndex, d[1] = that channel's messages JSON
        function onModuleEventReceived(m, e, d) {
            if (m !== "meshtastic_gateway") return
            if (e === "nodeStatus")           root.applyStatus(d[0])
            else if (e === "channelsChanged") root.applyChannels(d[0])
            else if (e === "messagesChanged") root.applyMessages(d[0], d[1])
            else if (e === "nodesChanged")    root.applyNodes(d[0])
            else if (e === "settingsChanged") root.applySettings(d[0])
            else if (e === "ownerSaved")      { root.ownerSaved = true; ownerSavedTimer.restart() }
        }
    }

    ListModel { id: channels }

    Rectangle {
        anchors.fill: parent
        color: root.t.bg

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // ══════════ SIDEBAR ══════════════════════════════════════
            Rectangle {
                Layout.preferredWidth: 220
                Layout.fillHeight: true
                color: root.t.bgElev

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: root.t.md
                    spacing: root.t.md

                    // logo + title
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm
                        Canvas {
                            width: 26; height: 26
                            Layout.alignment: Qt.AlignVCenter
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width / 2, cy = height / 2, r = 9
                                var pts = []
                                for (var i = 0; i < 5; i++) {
                                    var a = -Math.PI / 2 + i * 2 * Math.PI / 5
                                    pts.push({ x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) })
                                }
                                ctx.strokeStyle = root.t.primary; ctx.lineWidth = 1.2; ctx.globalAlpha = 0.7
                                for (i = 0; i < pts.length; i++) { ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(pts[i].x, pts[i].y); ctx.stroke() }
                                ctx.globalAlpha = 1
                                for (i = 0; i < pts.length; i++) {
                                    ctx.fillStyle = (i % 2 === 0) ? root.t.primary : root.t.mesh
                                    ctx.beginPath(); ctx.arc(pts[i].x, pts[i].y, 2, 0, 2 * Math.PI); ctx.fill()
                                }
                                ctx.fillStyle = root.t.primary
                                ctx.beginPath(); ctx.arc(cx, cy, 3.5, 0, 2 * Math.PI); ctx.fill()
                            }
                        }
                        Text {
                            text: "Meshtastic"; color: root.t.text
                            font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        // settings gear
                        Rectangle {
                            width: 28; height: 28; radius: root.t.radius
                            Layout.alignment: Qt.AlignVCenter
                            color: gearHover.containsMouse ? root.t.bgSec : "transparent"
                            Text {
                                anchors.centerIn: parent; text: "⚙"
                                color: gearHover.containsMouse ? root.t.text : root.t.textSec
                                font.pixelSize: 16
                            }
                            MouseArea {
                                id: gearHover
                                anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.gw("requestSettings")
                                    longNameField.text = root.selfLongName()
                                    shortNameField.text = root.selfShortName()
                                    root.settingsOpen = true
                                }
                            }
                        }
                    }

                    // node status
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm
                        Rectangle {
                            id: linkDot
                            width: 8; height: 8; radius: 4
                            Layout.alignment: Qt.AlignVCenter
                            color: root.linkState === "connected" ? root.t.success
                                 : root.linkState === "connecting" ? root.t.warn
                                 : root.linkState === "noperm" ? root.t.danger : root.t.textMuted
                            // Blink while connecting (animate a helper prop, not a Canvas → cheap).
                            property real blink: 1.0
                            opacity: root.linkState === "connecting" ? blink : 1.0
                            SequentialAnimation {
                                running: root.linkState === "connecting"
                                loops: Animation.Infinite
                                NumberAnimation { target: linkDot; property: "blink"; to: 0.25; duration: 550; easing.type: Easing.InOutQuad }
                                NumberAnimation { target: linkDot; property: "blink"; to: 1.0;  duration: 550; easing.type: Easing.InOutQuad }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: root.t.textSec; font.pixelSize: root.t.fSmall
                            text: root.linkState === "connected" ? root.nodeName
                                : root.linkState === "connecting" ? "Connecting…"
                                : root.linkState === "noperm" ? "Serial port busy / no access"
                                : "No node connected"
                        }
                    }

                    // mesh node stats (from the node's NodeDB)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: root.nodePresent
                        Rectangle {
                            width: 7; height: 7; radius: 4
                            Layout.alignment: Qt.AlignVCenter
                            color: root.nodesOnline > 0 ? root.t.success : root.t.textMuted
                        }
                        Text {
                            Layout.fillWidth: true
                            color: root.t.textMuted; font.pixelSize: root.t.fSmall
                            text: root.nodesOnline + " of " + root.nodesTotal + " nodes online"
                        }
                    }

                    // Logos Messaging (delivery) status — the bridge's internet side
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm
                        Rectangle {
                            id: lmDot
                            width: 8; height: 8; radius: 4
                            Layout.alignment: Qt.AlignVCenter
                            // green = ready (consistent with the LoRa dot), amber blink = connecting, grey = offline
                            color: root.deliveryState === "ready" ? root.t.success
                                 : root.deliveryState === "connecting" ? root.t.warn : root.t.textMuted
                            property real blink: 1.0
                            opacity: root.deliveryState === "connecting" ? blink : 1.0
                            SequentialAnimation {
                                running: root.deliveryState === "connecting"
                                loops: Animation.Infinite
                                NumberAnimation { target: lmDot; property: "blink"; to: 0.25; duration: 550; easing.type: Easing.InOutQuad }
                                NumberAnimation { target: lmDot; property: "blink"; to: 1.0;  duration: 550; easing.type: Easing.InOutQuad }
                            }
                        }
                        Text {
                            Layout.fillWidth: true; elide: Text.ElideRight
                            color: root.t.textSec; font.pixelSize: root.t.fSmall
                            text: root.deliveryState === "ready" ? "Logos Messaging"
                                : root.deliveryState === "connecting" ? "Connecting Logos Messaging…"
                                : "Logos Messaging offline"
                        }
                    }

                    // view switch: Channels | Nodes
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [ { k: "chat", label: "Channels" }, { k: "nodes", label: "Nodes" } ]
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                height: 28; radius: root.t.radius
                                property bool active: root.view === modelData.k
                                color: active ? Qt.rgba(0.93, 0.48, 0.35, 0.16)
                                              : (navHover.containsMouse ? root.t.bgSec : "transparent")
                                border.width: active ? 1 : 0; border.color: root.t.primary
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    color: active ? root.t.primary : root.t.textSec
                                    font.pixelSize: root.t.fSmall
                                    font.weight: active ? Font.DemiBold : Font.Normal
                                }
                                MouseArea {
                                    id: navHover
                                    anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.view = modelData.k
                                }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: root.t.border }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            Layout.fillWidth: true
                            text: root.view === "chat" ? "CHANNELS" : (root.nodeList.length + " NODES")
                            color: root.t.textMuted
                            font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1
                        }
                        // add channel
                        Rectangle {
                            visible: root.view === "chat"
                            width: 20; height: 20; radius: root.t.radius
                            color: addChanHover.containsMouse ? root.t.bgSec : "transparent"
                            Text { anchors.centerIn: parent; text: "+"; color: root.t.textSec; font.pixelSize: 16 }
                            MouseArea {
                                id: addChanHover; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { newChanField.text = ""; importChanField.text = ""; root.createChanOpen = true }
                            }
                        }
                    }

                    // channel list
                    ListView {
                        id: chanList
                        visible: root.view === "chat"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: channels
                        spacing: 4
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            width: chanList.width
                            height: 50
                            radius: root.t.radius
                            property bool selected: model.channelIndex === root.openChannel
                            color: selected ? Qt.rgba(0.93, 0.48, 0.35, 0.16)
                                            : (rowHover.containsMouse ? root.t.bgSec : "transparent")
                            border.width: selected ? 1 : 0
                            border.color: root.t.primary

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                                spacing: root.t.sm

                                // encryption badge (color = PSK status, Meshtastic convention)
                                Rectangle {
                                    width: 12; height: 12; radius: 3
                                    Layout.alignment: Qt.AlignVCenter
                                    color: root.pskColor(model.pskStatus)
                                    Rectangle {   // little "keyhole"
                                        width: 3; height: 3; radius: 2; color: root.t.bgElev
                                        anchors.centerIn: parent
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true; elide: Text.ElideRight
                                        text: model.displayName; color: root.t.text
                                        font.pixelSize: root.t.fBody
                                        font.weight: model.unread > 0 ? Font.DemiBold : Font.Normal
                                    }
                                    Text {
                                        text: model.relaying ? "⇄ relaying" : "mesh only"
                                        color: model.relaying ? root.t.success : root.t.textMuted
                                        font.pixelSize: 10
                                    }
                                }

                                // unread badge
                                Rectangle {
                                    visible: model.unread > 0
                                    Layout.alignment: Qt.AlignVCenter
                                    radius: height / 2
                                    height: 18
                                    width: Math.max(18, unreadTxt.implicitWidth + 10)
                                    color: root.t.primary
                                    Text {
                                        id: unreadTxt; anchors.centerIn: parent
                                        text: model.unread; color: root.t.onPrimary
                                        font.pixelSize: 11; font.weight: Font.DemiBold
                                    }
                                }
                            }

                            MouseArea {
                                id: rowHover
                                anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectChannel(model.channelIndex)
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: channels.count === 0
                            width: parent.width - 20; horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: "No channels — connect a Meshtastic node"
                            color: root.t.textMuted; font.pixelSize: root.t.fSmall
                        }
                    }

                    // node list
                    ListView {
                        id: nodeListView
                        visible: root.view === "nodes"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.nodeList
                        spacing: 4
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            width: nodeListView.width
                            height: 50
                            radius: root.t.radius
                            property bool selected: modelData.num === root.selNodeNum
                            color: selected ? Qt.rgba(0.93, 0.48, 0.35, 0.16)
                                            : (nodeHover.containsMouse ? root.t.bgSec : "transparent")
                            border.width: selected ? 1 : 0
                            border.color: root.t.primary

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                                spacing: root.t.sm

                                Rectangle {   // online dot
                                    width: 8; height: 8; radius: 4
                                    Layout.alignment: Qt.AlignVCenter
                                    color: modelData.isSelf ? root.t.logos
                                         : modelData.online ? root.t.success : root.t.textMuted
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        Text {
                                            Layout.fillWidth: true; elide: Text.ElideRight
                                            text: modelData.name || ("!" + modelData.num.toString(16))
                                            color: root.t.text; font.pixelSize: root.t.fBody
                                        }
                                        Text {
                                            visible: modelData.isSelf
                                            text: "you"; color: root.t.logos; font.pixelSize: 10
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true; elide: Text.ElideRight
                                        color: root.t.textMuted; font.pixelSize: 10
                                        text: (modelData.isSelf ? "this node"
                                              : root.fmtAgo(modelData.lastHeardAgo))
                                            + (modelData.hopsAway !== undefined && !modelData.isSelf
                                              ? " · " + modelData.hopsAway + (modelData.hopsAway === 1 ? " hop" : " hops") : "")
                                            + (modelData.distanceM !== undefined ? " · " + root.fmtDist(modelData.distanceM) : "")
                                    }
                                }

                                Text {   // battery
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: modelData.battery !== undefined
                                    text: root.battLabel(modelData.battery)
                                    color: root.battColor(modelData.battery)
                                    font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold
                                }
                            }

                            MouseArea {
                                id: nodeHover
                                anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selNodeNum = modelData.num
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: root.nodeList.length === 0
                            width: parent.width - 20; horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: "No nodes yet — waiting for the mesh"
                            color: root.t.textMuted; font.pixelSize: root.t.fSmall
                        }
                    }
                }
            }

            Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: root.t.border }

            // ══════════ CHAT PANE ════════════════════════════════════
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.view === "chat"

                // ── empty state ──────────────────────────────────────
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - root.t.lg * 2, 420)
                    spacing: root.t.md
                    visible: root.openChannel < 0

                    Text {
                        Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: root.linkState === "noperm"
                                ? "Can't open the serial device.\nClose any other app using the node (or a second Basecamp), or grant your user serial access:\nsudo usermod -aG dialout $USER   — then log out and back in."
                            : root.nodePresent ? "Select a channel to start chatting"
                                               : "Waiting for a Meshtastic node…"
                        color: root.linkState === "noperm" ? root.t.warn : root.t.textSec
                        font.pixelSize: root.t.fBody
                    }
                    Text {
                        Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                        text: root.relayingCount + " of " + channels.count + " channels relaying to Logos Messaging"
                        color: root.t.textMuted; font.pixelSize: root.t.fSmall
                        visible: channels.count > 0
                    }
                }

                // ── active chat ──────────────────────────────────────
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: root.t.lg
                    spacing: root.t.md
                    visible: root.openChannel >= 0

                    // header
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.md

                        Rectangle {
                            width: 14; height: 14; radius: 3
                            Layout.alignment: Qt.AlignVCenter
                            color: root.pskColor(root.selPsk)
                            Rectangle { width: 4; height: 4; radius: 2; color: root.t.bg; anchors.centerIn: parent }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                text: root.selName; color: root.t.text
                                font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold
                            }
                            Text {
                                text: root.pskLabel(root.selPsk)
                                      + (root.selRelaying ? "  ·  relaying to Logos Messaging" : "")
                                color: root.selRelaying ? root.t.success : root.t.textMuted
                                font.pixelSize: root.t.fSmall
                            }
                        }

                        // relay toggle
                        Text { text: "relay"; color: root.t.textSec; font.pixelSize: root.t.fSmall
                               Layout.alignment: Qt.AlignVCenter }
                        Rectangle {
                            id: relayTog
                            Layout.alignment: Qt.AlignVCenter
                            width: 46; height: 26; radius: 13
                            color: root.selRelaying ? root.t.primary : root.t.border
                            Behavior on color { ColorAnimation { duration: 120 } }
                            Rectangle {
                                width: 20; height: 20; radius: 10; color: root.t.text
                                anchors.verticalCenter: parent.verticalCenter
                                x: root.selRelaying ? parent.width - width - 3 : 3
                                Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                            }
                            MouseArea {
                                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: root.toggleRelay(root.openChannel, !root.selRelaying)
                            }
                        }

                        // share channel
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            height: 26; radius: root.t.radius
                            width: shareTxt.implicitWidth + 18
                            color: shareHover.containsMouse ? root.t.bgSec : "transparent"
                            border.width: 1; border.color: root.t.border
                            Text { id: shareTxt; anchors.centerIn: parent; text: "Share"
                                   color: root.t.textSec; font.pixelSize: root.t.fSmall }
                            MouseArea {
                                id: shareHover; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { root.genQr(root.selShareUrl); root.shareOpen = true }
                            }
                        }

                        // delete channel (never the primary)
                        Rectangle {
                            visible: root.openChannel > 0
                            Layout.alignment: Qt.AlignVCenter
                            height: 26; radius: root.t.radius
                            width: delTxt.implicitWidth + 18
                            color: delHover.containsMouse ? Qt.rgba(0.9, 0.28, 0.3, 0.16) : "transparent"
                            border.width: 1; border.color: delHover.containsMouse ? root.t.danger : root.t.border
                            Text { id: delTxt; anchors.centerIn: parent; text: "Delete"
                                   color: root.t.danger; font.pixelSize: root.t.fSmall }
                            MouseArea {
                                id: delHover; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor; onClicked: root.confirmDelOpen = true
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: root.t.border }

                    // message list
                    ListView {
                        id: chatView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.msgList
                        spacing: root.t.sm
                        boundsBehavior: Flickable.StopAtBounds
                        onCountChanged: positionViewAtEnd()

                        delegate: Item {
                            id: msgDelegate
                            width: chatView.width
                            implicitHeight: col.implicitHeight + 2
                            property bool out: !!modelData.outgoing
                            property bool picking: false
                            property int messageId: modelData.id !== undefined ? modelData.id : -1
                            property var reactions: modelData.reactions || []

                            Column {
                                id: col
                                anchors.left: parent.left
                                anchors.right: parent.right
                                spacing: 3

                                // bubble
                                Item {
                                    width: parent.width
                                    height: bubble.height
                                    Rectangle {
                                        id: bubble
                                        // Size from the text's NATURAL (unwrapped) width capped at maxW.
                                        // maxW comes from chatView.width (external/stable), never from the
                                        // bubble itself — that's what avoids the collapse-to-zero cycle.
                                        property real maxW: chatView.width * 0.66
                                        property real bodyW: Math.min(bodyText.implicitWidth, maxW)
                                        property real nameW: msgDelegate.out ? 0 : nameText.implicitWidth
                                        anchors.right: msgDelegate.out ? parent.right : undefined
                                        anchors.left: msgDelegate.out ? undefined : parent.left
                                        width: Math.max(nameW, bodyW) + root.t.md * 2
                                        height: inner.implicitHeight + root.t.sm * 2
                                        radius: root.t.radius
                                        color: msgDelegate.out ? root.t.primary : root.t.bgSec

                                        Column {
                                            id: inner
                                            x: root.t.md; y: root.t.sm
                                            width: bubble.width - root.t.md * 2
                                            spacing: 2
                                            Text {
                                                id: nameText
                                                visible: !msgDelegate.out
                                                text: modelData.from; color: root.t.success
                                                font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold
                                            }
                                            Text {
                                                id: bodyText
                                                text: modelData.text
                                                // Wrap against the STABLE external cap only. Do NOT read our own
                                                // implicitWidth here (e.g. via bubble.bodyW or Math.min(implicitWidth,…)):
                                                // a wrapping Text's width feeds back into its implicitWidth → infinite
                                                // "Maximum call stack size exceeded" relayout. The bubble still shrinks
                                                // to fit short messages via its own width binding (Math.max(nameW,bodyW)).
                                                width: bubble.maxW
                                                color: msgDelegate.out ? root.t.onPrimary : root.t.text
                                                font.pixelSize: root.t.fBody; wrapMode: Text.Wrap
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: msgDelegate.picking = !msgDelegate.picking
                                        }
                                    }

                                    // react affordance
                                    Rectangle {
                                        width: 22; height: 22; radius: 11
                                        color: root.t.bgElev; border.color: root.t.border; border.width: 1
                                        anchors.verticalCenter: bubble.verticalCenter
                                        anchors.left: msgDelegate.out ? undefined : bubble.right
                                        anchors.right: msgDelegate.out ? bubble.left : undefined
                                        anchors.leftMargin: msgDelegate.out ? 0 : 6
                                        anchors.rightMargin: msgDelegate.out ? 6 : 0
                                        visible: bubbleHover.hovered || msgDelegate.picking
                                        Text { anchors.centerIn: parent; text: "☺"; color: root.t.textSec; font.pixelSize: 13 }
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                                    onClicked: msgDelegate.picking = !msgDelegate.picking }
                                    }
                                    HoverHandler { id: bubbleHover }
                                }

                                // emoji picker
                                Row {
                                    visible: msgDelegate.picking
                                    spacing: 4
                                    anchors.right: msgDelegate.out ? parent.right : undefined
                                    anchors.left: msgDelegate.out ? undefined : parent.left
                                    Repeater {
                                        model: root.reactChoices
                                        delegate: Rectangle {
                                            width: 30; height: 28; radius: 6
                                            color: emojiHover.containsMouse ? root.t.bgSec : root.t.bgElev
                                            border.color: root.t.border; border.width: 1
                                            Text { anchors.centerIn: parent; text: modelData; font.pixelSize: 15 }
                                            MouseArea {
                                                id: emojiHover; anchors.fill: parent; hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: { root.react(msgDelegate.messageId, modelData); msgDelegate.picking = false }
                                            }
                                        }
                                    }
                                }

                                // existing reactions
                                Row {
                                    visible: msgDelegate.reactions.length > 0
                                    spacing: 4
                                    anchors.right: msgDelegate.out ? parent.right : undefined
                                    anchors.left: msgDelegate.out ? undefined : parent.left
                                    Repeater {
                                        model: msgDelegate.reactions
                                        delegate: Rectangle {
                                            height: 20; radius: 10
                                            width: chipRow.implicitWidth + 12
                                            color: root.t.bgElev; border.color: root.t.border; border.width: 1
                                            Row {
                                                id: chipRow; anchors.centerIn: parent; spacing: 3
                                                Text { text: modelData.emoji; font.pixelSize: 12 }
                                                Text { text: modelData.count; color: root.t.textSec; font.pixelSize: 11 }
                                            }
                                        }
                                    }
                                }

                                // meta: delivery ack (outgoing) + relay/origin tag (both directions)
                                Item {
                                    width: parent.width
                                    height: metaRow.visible ? metaRow.implicitHeight : 0
                                    Row {
                                        id: metaRow
                                        spacing: 8
                                        anchors.right: msgDelegate.out ? parent.right : undefined
                                        anchors.left: msgDelegate.out ? undefined : parent.left
                                        visible: (msgDelegate.out && root.ackGlyph(modelData.ackStatus) !== "")
                                                 || root.relayText(modelData) !== ""
                                        Text {
                                            visible: msgDelegate.out && root.ackGlyph(modelData.ackStatus) !== ""
                                            text: root.ackGlyph(modelData.ackStatus) + " " + root.ackLabel(modelData.ackStatus)
                                            color: root.ackColor(modelData.ackStatus)
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            visible: root.relayText(modelData) !== ""
                                            text: root.relayText(modelData)
                                            color: root.relayColor(modelData)
                                            font.pixelSize: 10
                                        }
                                    }
                                }
                            }

                        }

                        Text {
                            anchors.centerIn: parent
                            visible: root.msgList.length === 0
                            text: "No messages yet — say hi"
                            color: root.t.textMuted; font.pixelSize: root.t.fSmall
                        }
                    }

                    // transient publish/send status banner
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.statusMsg !== "" ? 28 : 0
                        visible: root.statusMsg !== ""
                        radius: root.t.radius
                        color: Qt.rgba(0.93, 0.48, 0.35, 0.12)
                        border.color: root.t.primary; border.width: 1
                        opacity: root.statusMsg !== "" ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 180 } }
                        Row {
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                width: 7; height: 7; radius: 4; anchors.verticalCenter: parent.verticalCenter
                                color: root.t.primary
                                SequentialAnimation on opacity {
                                    running: root.statusMsg !== ""; loops: Animation.Infinite
                                    NumberAnimation { to: 0.3; duration: 500 }
                                    NumberAnimation { to: 1.0; duration: 500 }
                                }
                            }
                            Text { text: root.statusMsg; color: root.t.primary; font.pixelSize: root.t.fSmall }
                        }
                    }

                    // reply box
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm

                        Rectangle {
                            Layout.fillWidth: true
                            height: 40; radius: root.t.radius
                            color: root.t.bgElev
                            border.width: 1
                            border.color: input.activeFocus ? root.t.primary : root.t.border
                            TextField {
                                id: input
                                anchors.fill: parent
                                anchors.leftMargin: root.t.md; anchors.rightMargin: root.t.md
                                verticalAlignment: TextInput.AlignVCenter
                                color: root.t.text
                                font.pixelSize: root.t.fBody
                                placeholderText: "Message " + root.selName
                                placeholderTextColor: root.t.textMuted
                                background: Item {}
                                onAccepted: root.sendCurrent()
                            }
                        }
                        Rectangle {
                            width: 64; height: 40; radius: root.t.radius
                            color: input.text && input.text.trim() ? root.t.primary : root.t.border
                            Text {
                                anchors.centerIn: parent; text: "Send"
                                color: input.text && input.text.trim() ? root.t.onPrimary : root.t.textMuted
                                font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold
                            }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.sendCurrent() }
                        }
                    }
                }
            }

            // ══════════ NODES PANE (relative-plot map + detail, one shared selection) ══════════
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.view === "nodes"

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    // ── relative-plot map (fills remaining space) ──
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Canvas {
                            id: mapCanvas
                            anchors.fill: parent
                            // Static — repaints only on data/units/size change (applyNodes/applySettings).
                            property var hits: []   // [{num,x,y}] captured during paint for click hit-testing
                            onVisibleChanged: if (visible) requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var w = width, h = height
                                ctx.fillStyle = root.t.bg; ctx.fillRect(0, 0, w, h)
                                var cx = w / 2, cy = h / 2
                                var plotR = Math.max(40, Math.min(w, h) / 2 - 50)

                                var peers = [], maxD = 0, noPos = 0
                                for (var i = 0; i < root.nodeList.length; i++) {
                                    var n = root.nodeList[i]
                                    if (n.isSelf) continue
                                    if (n.distanceM === undefined || n.bearingDeg === undefined) { noPos++; continue }
                                    peers.push(n); if (n.distanceM > maxD) maxD = n.distanceM
                                }
                                if (maxD <= 0) maxD = 1000

                                // pick a "nice" ring step so ~3 rings cover the farthest node
                                function niceStep(maxv, rings) {
                                    var raw = maxv / rings
                                    var p10 = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10))
                                    var cand = [1, 2, 2.5, 5, 10]
                                    for (var k = 0; k < cand.length; k++) if (cand[k] * p10 >= raw) return cand[k] * p10
                                    return 10 * p10
                                }
                                // zoom: visibleMax = metres from centre to the plot edge (zoom in → smaller)
                                var visibleMax = Math.max(10, (maxD / root.mapZoom))
                                var scaleR = plotR / visibleMax           // px per metre
                                var step = niceStep(visibleMax, 3)

                                // range rings + distance labels (honor km/mi via fmtDist)
                                ctx.strokeStyle = root.t.border; ctx.lineWidth = 1; ctx.font = "10px sans-serif"
                                for (var rr = 1; rr * step * scaleR <= plotR + 0.5; rr++) {
                                    var rad = rr * step * scaleR
                                    ctx.globalAlpha = 0.5
                                    ctx.beginPath(); ctx.arc(cx, cy, rad, 0, 2 * Math.PI); ctx.stroke()
                                    ctx.globalAlpha = 1
                                    ctx.fillStyle = root.t.textMuted; ctx.textAlign = "left"
                                    ctx.fillText(root.fmtDist(rr * step), cx + 4, cy - rad - 3)
                                }
                                ctx.fillStyle = root.t.textSec; ctx.textAlign = "center"
                                ctx.fillText("N", cx, cy - plotR - 14)

                                // peers — highlight the selected one; clamp out-of-range nodes to the edge
                                var hits = []
                                for (i = 0; i < peers.length; i++) {
                                    var pn = peers[i]
                                    var ang = pn.bearingDeg * Math.PI / 180
                                    var rawR = pn.distanceM * scaleR
                                    var off = rawR > plotR                  // beyond the current zoom window
                                    var pr = off ? plotR : rawR
                                    var px = cx + pr * Math.sin(ang)
                                    var py = cy - pr * Math.cos(ang)
                                    var isSel = pn.num === root.selNodeNum
                                    if (isSel) {
                                        ctx.strokeStyle = root.t.primary; ctx.lineWidth = 2
                                        ctx.beginPath(); ctx.arc(px, py, 9, 0, 2 * Math.PI); ctx.stroke()
                                    }
                                    ctx.globalAlpha = off ? 0.45 : 1.0      // dim clamped (out-of-range) dots
                                    ctx.fillStyle = pn.online ? root.t.success : root.t.textMuted
                                    ctx.beginPath(); ctx.arc(px, py, off ? 3 : 5, 0, 2 * Math.PI); ctx.fill()
                                    ctx.globalAlpha = 1.0
                                    if (!off) {
                                        ctx.fillStyle = isSel ? root.t.text : root.t.textSec; ctx.textAlign = "center"
                                        ctx.fillText(pn.name || "", px, py - 9)
                                    }
                                    hits.push({ num: pn.num, x: px, y: py })
                                }
                                mapCanvas.hits = hits

                                // our node at the centre
                                ctx.fillStyle = root.t.logos
                                ctx.beginPath(); ctx.arc(cx, cy, 6, 0, 2 * Math.PI); ctx.fill()
                                ctx.fillStyle = root.t.textSec; ctx.textAlign = "center"
                                ctx.fillText("you", cx, cy + 18)

                                ctx.fillStyle = root.t.textMuted; ctx.textAlign = "center"; ctx.font = "11px sans-serif"
                                ctx.fillText(peers.length + " positioned · " + noPos + " without position", cx, h - 12)
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: function (mouse) {
                                    var best = -1, bestd = 18 * 18   // within ~18px
                                    for (var i = 0; i < mapCanvas.hits.length; i++) {
                                        var dx = mouse.x - mapCanvas.hits[i].x, dy = mouse.y - mapCanvas.hits[i].y
                                        var d = dx * dx + dy * dy
                                        if (d < bestd) { bestd = d; best = mapCanvas.hits[i].num }
                                    }
                                    root.selNodeNum = (best !== -1) ? best : -1   // click empty space = deselect
                                }
                                onWheel: function (wheel) {
                                    root.zoomMap(wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15)
                                }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: root.nodeList.length === 0
                            text: "Waiting for the mesh…"
                            color: root.t.textMuted; font.pixelSize: root.t.fBody
                        }

                        // zoom controls
                        Column {
                            anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: root.t.sm
                            spacing: 4
                            visible: root.nodeList.length > 0
                            Repeater {
                                model: [ { l: "+", a: "in" }, { l: "−", a: "out" }, { l: "⊙", a: "fit" } ]
                                delegate: Rectangle {
                                    width: 28; height: 28; radius: root.t.radius
                                    color: zbHover.containsMouse ? root.t.bgSec : root.t.bgElev
                                    border.width: 1; border.color: root.t.border
                                    Text {
                                        anchors.centerIn: parent; text: modelData.l
                                        color: root.t.textSec; font.pixelSize: 15
                                    }
                                    MouseArea {
                                        id: zbHover; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: modelData.a === "in" ? root.zoomMap(1.3)
                                                 : modelData.a === "out" ? root.zoomMap(1 / 1.3)
                                                 : root.resetZoom()
                                    }
                                }
                            }
                        }
                    }

                    // ── detail panel (right; appears when a node is selected) ──
                    Rectangle {
                        Layout.preferredWidth: 300
                        Layout.fillHeight: true
                        visible: root.selectedNode() !== null
                        color: root.t.bgElev

                        Rectangle { width: 1; height: parent.height; color: root.t.border }  // left divider

                        ColumnLayout {
                            id: detailCol
                            anchors.fill: parent
                            anchors.margins: root.t.lg
                            spacing: root.t.md
                            property var n: root.selectedNode() || ({})

                            // header (with × to deselect)
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: root.t.sm
                                Rectangle {
                                    width: 12; height: 12; radius: 6
                                    Layout.alignment: Qt.AlignVCenter
                                    color: detailCol.n.isSelf ? root.t.logos
                                         : detailCol.n.online ? root.t.success : root.t.textMuted
                                }
                                Text {
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                    text: detailCol.n.name || "node"
                                    color: root.t.text; font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold
                                }
                                Text {
                                    visible: detailCol.n.isFavorite === true
                                    text: "★"; color: root.t.warn; font.pixelSize: root.t.fTitle
                                }
                                Rectangle {
                                    width: 24; height: 24; radius: root.t.radius
                                    color: closeNodeHover.containsMouse ? root.t.bgSec : "transparent"
                                    Text { anchors.centerIn: parent; text: "✕"; color: root.t.textSec; font.pixelSize: 13 }
                                    MouseArea {
                                        id: closeNodeHover; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor; onClicked: root.selNodeNum = -1
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true; elide: Text.ElideRight
                                visible: (detailCol.n.longName || "") !== "" && detailCol.n.longName !== detailCol.n.name
                                text: detailCol.n.longName || ""
                                color: root.t.textSec; font.pixelSize: root.t.fBody
                            }

                            Rectangle { Layout.fillWidth: true; height: 1; color: root.t.border }

                            Flickable {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                contentHeight: statsCol.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                ColumnLayout {
                                    id: statsCol
                                    width: parent.width
                                    spacing: root.t.sm
                                    Repeater {
                                        model: root.nodeStatRows(detailCol.n)
                                        delegate: RowLayout {
                                            Layout.fillWidth: true
                                            spacing: root.t.md
                                            Text {
                                                Layout.preferredWidth: 110
                                                text: modelData.label; color: root.t.textMuted
                                                font.pixelSize: root.t.fSmall
                                            }
                                            Text {
                                                Layout.fillWidth: true; wrapMode: Text.Wrap
                                                text: modelData.value
                                                color: modelData.color !== undefined ? modelData.color : root.t.text
                                                font.pixelSize: root.t.fBody
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ══════════ SETTINGS MODAL ════════════════════════════════════
    Rectangle {
        anchors.fill: parent
        visible: root.settingsOpen
        color: Qt.rgba(0, 0, 0, 0.55)
        z: 100
        // click the backdrop to dismiss
        MouseArea { anchors.fill: parent; onClicked: root.settingsOpen = false }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(480, parent.width - 2 * root.t.lg)
            height: panelCol.implicitHeight + 2 * root.t.lg
            radius: root.t.radius
            color: root.t.bgElev
            border.width: 1; border.color: root.t.border
            MouseArea { anchors.fill: parent }   // swallow clicks so they don't reach the backdrop

            ColumnLayout {
                id: panelCol
                anchors.fill: parent
                anchors.margins: root.t.lg
                spacing: root.t.lg

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Settings"; color: root.t.text
                        font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 26; height: 26; radius: root.t.radius
                        color: closeHover.containsMouse ? root.t.bgSec : "transparent"
                        Text { anchors.centerIn: parent; text: "✕"; color: root.t.textSec; font.pixelSize: 14 }
                        MouseArea {
                            id: closeHover; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: root.settingsOpen = false
                        }
                    }
                }

                // ── node configuration (written to the radio) ──
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: root.t.sm
                    Text { text: "Node name (written to the radio)"; color: root.t.textSec; font.pixelSize: root.t.fSmall }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.t.sm
                        Rectangle {
                            Layout.fillWidth: true; height: 36; radius: root.t.radius
                            color: root.t.bg; border.width: 1
                            border.color: longNameField.activeFocus ? root.t.primary : root.t.border
                            TextField {
                                id: longNameField; anchors.fill: parent
                                anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                                verticalAlignment: TextInput.AlignVCenter
                                color: root.t.text; font.pixelSize: root.t.fBody
                                placeholderText: "Long name"; placeholderTextColor: root.t.textMuted
                                background: Item {}
                                maximumLength: 39
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 72; height: 36; radius: root.t.radius
                            color: root.t.bg; border.width: 1
                            border.color: shortNameField.activeFocus ? root.t.primary : root.t.border
                            TextField {
                                id: shortNameField; anchors.fill: parent
                                anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                                verticalAlignment: TextInput.AlignVCenter; horizontalAlignment: TextInput.AlignHCenter
                                color: root.t.text; font.pixelSize: root.t.fBody
                                placeholderText: "Short"; placeholderTextColor: root.t.textMuted
                                background: Item {}
                                maximumLength: 4
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 64; height: 36; radius: root.t.radius
                            property bool ok: longNameField.text && longNameField.text.trim().length > 0
                            color: ok ? root.t.primary : root.t.border
                            Text {
                                anchors.centerIn: parent; text: "Save"
                                color: parent.ok ? root.t.onPrimary : root.t.textMuted
                                font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold
                            }
                            MouseArea {
                                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                enabled: parent.ok
                                onClicked: root.setOwner(longNameField.text, shortNameField.text)
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: root.ownerSaved ? "✓ Saved — name written to the radio"
                                              : "Short name is up to 4 characters, shown as your tag on other nodes."
                        color: root.ownerSaved ? "#3fb950" : root.t.textMuted
                        font.pixelSize: 10; font.weight: root.ownerSaved ? Font.DemiBold : Font.Normal
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: root.t.border }

                Repeater {
                    model: [
                        { title: "Message history kept per channel", key: "maxMsgsPerChannel",
                          options: [ {l:"500",v:500}, {l:"2,000",v:2000}, {l:"10,000",v:10000}, {l:"Unlimited",v:0} ] },
                        { title: "Consider a node “online” if heard within", key: "onlineWindowSec",
                          options: [ {l:"15 min",v:900}, {l:"1 hour",v:3600}, {l:"2 hours",v:7200}, {l:"8 hours",v:28800}, {l:"24 hours",v:86400} ] },
                        { title: "Distance units", key: "distanceUnit",
                          options: [ {l:"Kilometres",v:"km"}, {l:"Miles",v:"mi"} ] }
                    ]
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm
                        property string settingKey: modelData.key
                        property var settingOptions: modelData.options
                        Text { text: modelData.title; color: root.t.textSec; font.pixelSize: root.t.fSmall }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: settingOptions
                                delegate: Rectangle {
                                    height: 30; radius: root.t.radius
                                    width: chipTxt.implicitWidth + 22
                                    property bool sel: root.settings[settingKey] === modelData.v
                                    color: sel ? Qt.rgba(0.93, 0.48, 0.35, 0.16)
                                               : (chipHover.containsMouse ? root.t.bgSec : "transparent")
                                    border.width: 1; border.color: sel ? root.t.primary : root.t.border
                                    Text {
                                        id: chipTxt; anchors.centerIn: parent; text: modelData.l
                                        color: sel ? root.t.primary : root.t.textSec; font.pixelSize: root.t.fSmall
                                    }
                                    MouseArea {
                                        id: chipHover; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.setSetting(settingKey, modelData.v)
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "History is kept in a local database; the cap only limits how much is retained. Changes apply immediately."
                    color: root.t.textMuted; font.pixelSize: 10
                }
            }
        }
    }

    // ══════════ SHARE CHANNEL MODAL ═══════════════════════════════
    Rectangle {
        anchors.fill: parent; visible: root.shareOpen; z: 100
        color: Qt.rgba(0, 0, 0, 0.55)
        MouseArea { anchors.fill: parent; onClicked: root.shareOpen = false }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(480, parent.width - 2 * root.t.lg)
            height: shareCol.implicitHeight + 2 * root.t.lg
            radius: root.t.radius; color: root.t.bgElev; border.width: 1; border.color: root.t.border
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: shareCol
                anchors.fill: parent; anchors.margins: root.t.lg; spacing: root.t.md
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true; elide: Text.ElideRight
                        text: "Share " + root.selName; color: root.t.text
                        font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold
                    }
                    Rectangle {
                        width: 26; height: 26; radius: root.t.radius
                        color: scHover.containsMouse ? root.t.bgSec : "transparent"
                        Text { anchors.centerIn: parent; text: "✕"; color: root.t.textSec; font.pixelSize: 14 }
                        MouseArea { id: scHover; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: root.shareOpen = false }
                    }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "Scan this QR in the Meshtastic app, or copy the link below, to join the channel:"
                    color: root.t.textSec; font.pixelSize: root.t.fSmall
                }
                // QR of the share URL — matrix from the `qr` core module, rendered inline as a grid
                // (no sibling component, so no QML type-resolution risk).
                Rectangle {
                    id: qrFrame
                    visible: root.qrN > 0
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 240; Layout.preferredHeight: 240
                    radius: 8; color: "#FFFFFF"
                    readonly property int cell: root.qrN > 0 ? Math.max(1, Math.floor((width - 24) / root.qrN)) : 1
                    Grid {
                        anchors.centerIn: parent
                        columns: root.qrN; rows: root.qrN
                        Repeater {
                            model: root.qrCells
                            delegate: Rectangle {
                                width: qrFrame.cell; height: qrFrame.cell
                                color: modelData ? "#000000" : "#FFFFFF"
                            }
                        }
                    }
                }
                Text {
                    visible: root.qrN === 0
                    Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                    text: "QR unavailable — the ‘qr’ module isn’t loaded."
                    color: root.t.textMuted; font.pixelSize: 11
                }
                Rectangle {
                    Layout.fillWidth: true; height: 66; radius: root.t.radius
                    color: root.t.bg; border.width: 1; border.color: root.t.border
                    TextEdit {
                        id: shareUrlField
                        anchors.fill: parent; anchors.margins: root.t.sm
                        text: root.selShareUrl; color: root.t.text
                        font.pixelSize: root.t.fSmall; font.family: "monospace"
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 90; height: 34; radius: root.t.radius; color: root.t.primary
                        Text { anchors.centerIn: parent; text: "Copy"; color: root.t.onPrimary
                               font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { shareUrlField.selectAll(); shareUrlField.copy(); shareUrlField.deselect() } }
                    }
                }
            }
        }
    }

    // ══════════ CREATE CHANNEL MODAL ══════════════════════════════
    Rectangle {
        anchors.fill: parent; visible: root.createChanOpen; z: 100
        color: Qt.rgba(0, 0, 0, 0.55)
        MouseArea { anchors.fill: parent; onClicked: root.createChanOpen = false }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 2 * root.t.lg)
            height: createCol.implicitHeight + 2 * root.t.lg
            radius: root.t.radius; color: root.t.bgElev; border.width: 1; border.color: root.t.border
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: createCol
                anchors.fill: parent; anchors.margins: root.t.lg; spacing: root.t.md
                Text { text: "Add channel"; color: root.t.text
                       font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold }

                // ── create a new channel (fresh random key) ──
                Text { Layout.fillWidth: true; wrapMode: Text.Wrap
                       text: "Create a new private channel (a fresh random key is generated):"
                       color: root.t.textSec; font.pixelSize: root.t.fSmall }
                RowLayout {
                    Layout.fillWidth: true; spacing: root.t.sm
                    Rectangle {
                        Layout.fillWidth: true; height: 38; radius: root.t.radius; color: root.t.bg
                        border.width: 1; border.color: newChanField.activeFocus ? root.t.primary : root.t.border
                        TextField {
                            id: newChanField; anchors.fill: parent
                            anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                            verticalAlignment: TextInput.AlignVCenter; color: root.t.text; font.pixelSize: root.t.fBody
                            placeholderText: "Channel name"; placeholderTextColor: root.t.textMuted
                            background: Item {}
                            maximumLength: 11
                            onAccepted: root.doCreateChannel(newChanField.text)
                        }
                    }
                    Rectangle {
                        property bool ok: newChanField.text && newChanField.text.trim().length > 0
                        width: 84; height: 38; radius: root.t.radius; color: ok ? root.t.primary : root.t.border
                        Text { anchors.centerIn: parent; text: "Create"; color: parent.ok ? root.t.onPrimary : root.t.textMuted
                               font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold }
                        MouseArea { anchors.fill: parent; enabled: parent.ok; cursorShape: Qt.PointingHandCursor
                            onClicked: root.doCreateChannel(newChanField.text) }
                    }
                }

                // ── or join an existing one from a share link ──
                RowLayout {
                    Layout.fillWidth: true; spacing: root.t.sm
                    Rectangle { Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter; height: 1; color: root.t.border }
                    Text { text: "or"; color: root.t.textMuted; font.pixelSize: root.t.fSmall }
                    Rectangle { Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter; height: 1; color: root.t.border }
                }
                Text { Layout.fillWidth: true; wrapMode: Text.Wrap
                       text: "Join an existing channel — paste a meshtastic.org/e/# share link:"
                       color: root.t.textSec; font.pixelSize: root.t.fSmall }
                RowLayout {
                    Layout.fillWidth: true; spacing: root.t.sm
                    Rectangle {
                        Layout.fillWidth: true; height: 38; radius: root.t.radius; color: root.t.bg
                        border.width: 1; border.color: importChanField.activeFocus ? root.t.primary : root.t.border
                        TextField {
                            id: importChanField; anchors.fill: parent
                            anchors.leftMargin: root.t.sm; anchors.rightMargin: root.t.sm
                            verticalAlignment: TextInput.AlignVCenter; color: root.t.text; font.pixelSize: root.t.fSmall
                            placeholderText: "https://meshtastic.org/e/#…"; placeholderTextColor: root.t.textMuted
                            background: Item {}
                            onAccepted: root.doImportChannel(importChanField.text)
                        }
                    }
                    Rectangle {
                        property bool ok: importChanField.text && importChanField.text.indexOf("#") >= 0
                        width: 84; height: 38; radius: root.t.radius; color: ok ? root.t.primary : root.t.border
                        Text { anchors.centerIn: parent; text: "Add"; color: parent.ok ? root.t.onPrimary : root.t.textMuted
                               font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold }
                        MouseArea { anchors.fill: parent; enabled: parent.ok; cursorShape: Qt.PointingHandCursor
                            onClicked: root.doImportChannel(importChanField.text) }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 80; height: 34; radius: root.t.radius; color: "transparent"
                        border.width: 1; border.color: root.t.border
                        Text { anchors.centerIn: parent; text: "Close"; color: root.t.textSec; font.pixelSize: root.t.fSmall }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.createChanOpen = false }
                    }
                }
            }
        }
    }

    // ══════════ DELETE CHANNEL CONFIRM ════════════════════════════
    Rectangle {
        anchors.fill: parent; visible: root.confirmDelOpen; z: 100
        color: Qt.rgba(0, 0, 0, 0.55)
        MouseArea { anchors.fill: parent; onClicked: root.confirmDelOpen = false }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(400, parent.width - 2 * root.t.lg)
            height: delCol.implicitHeight + 2 * root.t.lg
            radius: root.t.radius; color: root.t.bgElev; border.width: 1; border.color: root.t.border
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: delCol
                anchors.fill: parent; anchors.margins: root.t.lg; spacing: root.t.md
                Text { text: "Delete channel?"; color: root.t.text
                       font.pixelSize: root.t.fTitle; font.weight: Font.DemiBold }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "“" + root.selName + "” will be disabled on the radio. Stored history stays in the database."
                    color: root.t.textSec; font.pixelSize: root.t.fSmall
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: root.t.sm
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 80; height: 34; radius: root.t.radius; color: "transparent"
                        border.width: 1; border.color: root.t.border
                        Text { anchors.centerIn: parent; text: "Cancel"; color: root.t.textSec; font.pixelSize: root.t.fSmall }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.confirmDelOpen = false }
                    }
                    Rectangle {
                        width: 90; height: 34; radius: root.t.radius; color: root.t.danger
                        Text { anchors.centerIn: parent; text: "Delete"; color: "#ffffff"
                               font.pixelSize: root.t.fSmall; font.weight: Font.DemiBold }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.doDeleteChannel() }
                    }
                }
            }
        }
    }
}
