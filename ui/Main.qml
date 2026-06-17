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
    property var msgList: []                // JS array of message objects for the open channel
    property string statusMsg: ""           // transient "publishing…" banner text

    readonly property var reactChoices: ["👍", "❤️", "😂", "🔥", "‼️"]

    readonly property var t: ({
        bg:        "#171717", bgSec: "#262626", bgElev: "#0E121B",
        text:      "#FFFFFF", textSec: "#A4A4A4", textMuted: "#5C5C5C",
        border:    "#2B303B", primary: "#ED7B58", primaryHi: "#F55702",
        success:   "#6CCC93", mesh: "#6CCC93", logos: "#ED7B58",
        warn:      "#E0C341", danger: "#E5484D", onPrimary: "#1A0F0A",
        sm: 8, md: 12, lg: 16, radius: 8, fTitle: 18, fBody: 14, fSmall: 12, fMono: 11
    })

    // ── gateway bridge ────────────────────────────────────────────
    function gw(method, args) {
        if (typeof logos === "undefined" || !logos.callModule) return null
        return logos.callModule("meshtastic_gateway", method, args || [])
    }

    function refresh() {
        try {
            var st = JSON.parse(gw("status"))
            root.nodePresent = !!st.nodePresent
            root.nodeName = st.nodeName || ""
            root.nodesTotal = st.nodesTotal || 0
            root.nodesOnline = st.nodesOnline || 0
            var chans = JSON.parse(gw("getChannels"))
            channels.clear()
            var rc = 0
            for (var i = 0; i < chans.length; i++) {
                channels.append(chans[i])
                if (chans[i].relaying) rc++
            }
            root.relayingCount = rc
            syncSelected()
            net.requestPaint()
        } catch (e) { /* gateway not ready yet */ }
    }

    function syncSelected() {
        if (root.openChannel < 0) return
        for (var i = 0; i < channels.count; i++) {
            var c = channels.get(i)
            if (c.channelIndex === root.openChannel) {
                root.selName = c.displayName; root.selPsk = c.pskStatus
                root.selRelaying = c.relaying; root.selTopic = c.topic
                return
            }
        }
    }

    function selectChannel(idx) {
        root.openChannel = idx
        syncSelected()
        try { gw("markRead", [idx]) } catch (e) {}   // clears unread badge
        loadMessages()
    }
    function closeChat() { root.openChannel = -1 }

    function loadMessages() {
        if (root.openChannel < 0) return
        try {
            // A failed/re-entrant IPC returns null/"" — do NOT overwrite the list with that, or the
            // whole chat blanks out. Only replace when we actually got a valid array back.
            var raw = gw("getMessages", [root.openChannel])
            if (raw === null || raw === undefined || raw === "") return
            var parsed = JSON.parse(raw)
            if (Array.isArray(parsed)) {
                root.msgList = parsed
                Qt.callLater(function () { chatView.positionViewAtEnd() })
            }
        } catch (e) { /* keep the current list */ }
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

        // Defer the BLOCKING sync IPC to the next tick so the optimistic bubble paints first —
        // otherwise callModule() stalls the UI thread before anything renders (the "freeze").
        Qt.callLater(function () { try { gw("sendMessage", [root.openChannel, text]) } catch (e) {} })
    }

    function flashStatus(msg) { root.statusMsg = msg; statusTimer.restart() }

    function react(messageId, emoji) {
        try { gw("addReaction", [root.openChannel, messageId, emoji]) } catch (e) {}
    }

    function toggleRelay(idx, enabled) {
        try { gw("setRelay", [idx, enabled]) } catch (e) {}
    }

    // ── small view helpers ────────────────────────────────────────
    function ackGlyph(s) { return s === "delivered" ? "✓✓" : s === "enroute" ? "◌"
                                : s === "queued" ? "◌" : s === "failed" ? "✕" : "" }
    function ackColor(s) { return s === "delivered" ? t.success : s === "failed" ? t.danger : t.textSec }
    function ackLabel(s) { return s === "delivered" ? "delivered" : s === "enroute" ? "sending…"
                                : s === "queued" ? "queued" : s === "failed" ? "failed" : "" }
    function pskColor(s) { return s === "custom" ? t.success : s === "default" ? t.warn : t.danger }
    function pskLabel(s) { return s === "custom" ? "custom PSK" : s === "default" ? "default PSK" : "no PSK" }

    // per-message relay/origin tag (distinct from the mesh delivery ack)
    function relayText(msg) {
        if (msg.origin === "lm") return "⇄ from Logos Messaging"
        if (msg.relayed) return "⇄ relayed to Logos Messaging"
        return ""
    }
    function relayColor(msg) { return msg.origin === "lm" ? t.logos : t.success }

    // ── events ────────────────────────────────────────────────────
    Component.onCompleted: {
        if (typeof logos !== "undefined" && logos.onModuleEvent) {
            logos.onModuleEvent("meshtastic_gateway", "channelsChanged")
            logos.onModuleEvent("meshtastic_gateway", "messagesChanged")
        }
    }
    Timer {
        interval: 700; repeat: true; running: true
        onTriggered: { root.refresh(); if (root.nodePresent && channels.count > 0) running = false }
    }
    Timer { id: statusTimer; interval: 2600; onTriggered: root.statusMsg = "" }
    Connections {
        target: typeof logos !== "undefined" ? logos : null
        function onModuleEventReceived(m, e, d) {
            if (m !== "meshtastic_gateway") return
            if (e === "channelsChanged") root.refresh()
            else if (e === "messagesChanged") {
                // Defer off the current call stack: this event can arrive INSIDE a sync callModule's
                // nested event loop (e.g. during sendMessage). Calling getMessages there re-enters
                // the same replica and times out. Qt.callLater runs it on a clean tick.
                if (root.openChannel >= 0 && (d === undefined || d.length === 0 || d[0] === root.openChannel))
                    Qt.callLater(root.loadMessages)
            }
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
                    }

                    // node status
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.t.sm
                        Rectangle {
                            width: 7; height: 7; radius: 4
                            Layout.alignment: Qt.AlignVCenter
                            color: root.nodePresent ? root.t.success : root.t.textMuted
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: root.t.textSec; font.pixelSize: root.t.fSmall
                            text: root.nodePresent ? root.nodeName : "No node connected"
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

                    Rectangle { Layout.fillWidth: true; height: 1; color: root.t.border }

                    Text {
                        text: "CHANNELS"; color: root.t.textMuted
                        font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1
                    }

                    // channel list
                    ListView {
                        id: chanList
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
                }
            }

            Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: root.t.border }

            // ══════════ CHAT PANE ════════════════════════════════════
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // ── empty state: p2p bridge viz ──────────────────────
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - root.t.lg * 2, 420)
                    spacing: root.t.md
                    visible: root.openChannel < 0

                    Canvas {
                        id: net
                        Layout.fillWidth: true
                        Layout.preferredHeight: 130
                        property real phase: 0
                        NumberAnimation on phase { from: 0; to: 1; duration: 2200; loops: Animation.Infinite; running: true }
                        onPhaseChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset()
                            var w = width, h = height, cx = w / 2, cy = h / 2
                            var n = Math.max(3, Math.min(channels.count, 5))
                            var active = root.relayingCount
                            var mesh = [], lg = []
                            for (var i = 0; i < n; i++) {
                                var ay = h * 0.18 + (h * 0.64) * (n === 1 ? 0.5 : i / (n - 1))
                                mesh.push({ x: w * 0.14, y: ay }); lg.push({ x: w * 0.86, y: ay })
                            }
                            var g = { x: cx, y: cy }
                            function link(a, b, color, on) {
                                ctx.globalAlpha = 0.3; ctx.strokeStyle = color; ctx.lineWidth = 1.2
                                ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke(); ctx.globalAlpha = 1
                                if (on) {
                                    var px = a.x + (b.x - a.x) * net.phase, py = a.y + (b.y - a.y) * net.phase
                                    ctx.fillStyle = color; ctx.beginPath(); ctx.arc(px, py, 2.6, 0, 2 * Math.PI); ctx.fill()
                                }
                            }
                            for (i = 0; i < n; i++) { link(mesh[i], g, root.t.mesh, i < active); link(g, lg[i], root.t.logos, i < active) }
                            function dot(p, r, c) { ctx.fillStyle = c; ctx.beginPath(); ctx.arc(p.x, p.y, r, 0, 2 * Math.PI); ctx.fill() }
                            for (i = 0; i < n; i++) { dot(mesh[i], 3, root.t.mesh); dot(lg[i], 3, root.t.logos) }
                            ctx.shadowColor = root.t.primary; ctx.shadowBlur = 14; dot(g, 8, root.t.primary); ctx.shadowBlur = 0; dot(g, 3.5, root.t.bgElev)
                        }
                    }
                    Text {
                        Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                        text: root.nodePresent ? "Select a channel to start chatting"
                                               : "Waiting for a Meshtastic node…"
                        color: root.t.textSec; font.pixelSize: root.t.fBody
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
                                                width: bubble.bodyW
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
        }
    }
}
