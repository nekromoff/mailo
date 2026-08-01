// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami
import Mailo.Core

ColumnLayout {
    id: viewer
    spacing: 0

    /// The MessageContext this viewer renders. The reading pane binds
    /// Mail.readingContext; a detached message window owns its own context.
    /// All message state (bodies, attachments, junk flag, view URLs) comes
    /// from here, so several viewers can be on screen at once.
    property var context: null

    /// The uiSettings object from Main.qml (for the configurable shortcuts).
    /// Null is tolerated everywhere: the built-in defaults are used then.
    property var ui: null

    readonly property bool hasMessage: context ? context.hasMessage : false
    property string viewMode: "html"

    /// True while the find bar is open. The message window checks this so its
    /// Esc-closes-the-window shortcut does not swallow Esc-closes-the-find-bar.
    readonly property bool findActive: findBar.visible
    // Match counters, filled from findTextFinished (Chromium counts for us).
    property int findMatches: 0
    property int findCurrent: 0

    /// Reply / Reply all was clicked for the shown message.
    signal replyRequested(bool replyAll)
    /// Forward was clicked for the shown message.
    signal forwardRequested()

    // Reset to the "Select a message" placeholder (e.g. the shown message was
    // deleted and the list is now empty).
    function clear() {
        if (context)
            context.clear()
    }

    function showCurrent() {
        if (!context || !context.hasMessage) {
            web.url = "about:blank"
            return
        }
        // Junk folders open as plain text; the HTML button is the explicit
        // opt-in to render the (still sandboxed) HTML.
        viewMode = context.junkTextOnly ? "text" : "html"
        web.url = context.bodyUrl
    }

    /// Switch the rendered representation. Shared by the HTML/Text/Source
    /// buttons and the view-source shortcut so both stay in step.
    function showMode(mode) {
        if (!context || !context.hasMessage)
            return
        viewMode = mode
        web.url = mode === "text" ? context.textViewUrl()
                : mode === "source" ? context.sourceViewUrl()
                                    : context.htmlViewUrl()
    }

    /// Ctrl+U: source on, and off again back to the rendered message.
    function toggleSource() {
        if (!hasMessage)
            return
        showMode(viewMode === "source"
                 ? (context.junkTextOnly ? "text" : "html")
                 : "source")
    }

    function openFind() {
        if (!hasMessage)
            return
        findBar.visible = true
        findField.forceActiveFocus()
        findField.selectAll() // repeat presses replace the old term
        if (findField.text.length > 0)
            findRun(false)
    }

    function closeFind() {
        findBar.visible = false
        web.findText("") // drop the highlighting
        findMatches = 0
        findCurrent = 0
        web.forceActiveFocus()
    }

    // Chromium's find-in-page advances to the next match on every repeated
    // call with the same term, so next/previous is the same call as the
    // initial search — only the direction flag differs.
    function findRun(backward) {
        if (findField.text.length === 0) {
            web.findText("")
            findMatches = 0
            findCurrent = 0
            return
        }
        let flags = 0
        if (findCase.checked)
            flags |= WebEngineView.FindCaseSensitively
        if (backward)
            flags |= WebEngineView.FindBackward
        web.findText(findField.text, flags)
    }

    Shortcut {
        sequences: [viewer.ui ? viewer.ui.shortcutFind : "Ctrl+F"]
        enabled: viewer.hasMessage
        onActivated: viewer.openFind()
    }
    Shortcut {
        sequences: [viewer.ui ? viewer.ui.shortcutSource : "Ctrl+U"]
        enabled: viewer.hasMessage
        onActivated: viewer.toggleSource()
    }
    // Esc closes the bar wherever focus sits (the field handles it itself, but
    // focus is usually back in the page after a jump to a match). The message
    // window disables its own Esc-closes-the-window while the bar is open, so
    // the two never compete for the key.
    Shortcut {
        sequence: "Esc"
        enabled: viewer.findActive
        onActivated: viewer.closeFind()
    }
    Shortcut {
        sequences: ["F3", "Ctrl+G"]
        enabled: viewer.findActive
        onActivated: viewer.findRun(false)
    }
    Shortcut {
        sequences: ["Shift+F3", "Ctrl+Shift+G"]
        enabled: viewer.findActive
        onActivated: viewer.findRun(true)
    }

    // The context outlives any one message: re-render whenever it presents a
    // different one (reading pane), and once at startup for a window whose
    // context was filled before the viewer existed.
    Connections {
        target: viewer.context
        function onMessageChanged() {
            viewer.showCurrent()
        }
    }
    Component.onCompleted: showCurrent()

    // "purelymail.com; spf=pass …; dkim=fail …" → "spf=pass · dkim=fail ❗"
    // Only the leading method=result of each ';'-delimited field is a verdict.
    // Everything after it echoes sender-supplied data — smtp.mailfrom=,
    // header.from=, reason= — so scanning the whole header would let a sender
    // put "dkim=pass" in this badge by putting it in their own envelope
    // address. Quoted strings and (comments) are dropped first for the same
    // reason: both can carry a ';' and hide a verdict behind it.
    function condenseAuth(authInfo) {
        if (!authInfo || authInfo.length === 0)
            return ""
        const cleaned = authInfo
            .replace(/"(?:[^"\\]|\\.)*"/g, '""')
            .replace(/\([^()]*\)/g, " ")
        const fields = cleaned.split(";")
        let verdicts = []
        for (let i = 1; i < fields.length; ++i) { // field 0 is the authserv-id
            const m = /^\s*(dkim|spf|dmarc)\s*=\s*([a-z]+)/i.exec(fields[i])
            if (m)
                verdicts.push(m[1].toLowerCase() + "=" + m[2].toLowerCase())
        }
        return verdicts
            .map(v => /(fail|permerror)$/.test(v) ? v + " ❗" : v)
            .join(" · ")
    }

    // Envelope header block above the preview
    GridLayout {
        Layout.fillWidth: true
        Layout.margins: Kirigami.Units.largeSpacing
        visible: viewer.hasMessage
        columns: 2
        columnSpacing: Kirigami.Units.largeSpacing
        rowSpacing: Kirigami.Units.smallSpacing / 2

        QQC2.Label { text: "From:"; opacity: 0.8 }
        RowLayout {
            Layout.fillWidth: true
            SelectableValue {
                id: fromLabel
                text: viewer.context ? viewer.context.from : ""
            }
            // Explicit arrow glyphs — theme icons for reply/forward are not
            // reliably recognizable as arrows.
            QQC2.ToolButton {
                text: "← Reply"
                onClicked: viewer.replyRequested(false)
                QQC2.ToolTip.text: "Reply to the sender"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                text: "⇇ Reply all"
                onClicked: viewer.replyRequested(true)
                QQC2.ToolTip.text: "Reply to the sender and all recipients"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                text: "Forward →"
                onClicked: viewer.forwardRequested()
                QQC2.ToolTip.text: "Forward this message"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "image-x-generic"
                text: "Load remote content"
                checkable: true
                checked: viewer.context ? viewer.context.remoteContentAllowed : false
                visible: viewer.viewMode === "html"
                onToggled: {
                    viewer.context.remoteContentAllowed = checked
                    web.url = viewer.context.htmlViewUrl() // re-render with the new policy
                }
                QQC2.ToolTip.text: "Allow this message to load remote images, styles and fonts (JavaScript stays off)"
                QQC2.ToolTip.visible: hovered
            }
            SelectableValue {
                id: dateLabel
                // Fixed trailing item: it is short enough to always fit, so it
                // keeps its own width instead of competing for the row's.
                Layout.fillWidth: false
                Layout.preferredWidth: -1
                opacity: 0.8
                text: viewer.context ? viewer.context.date : ""
            }
        }

        // Captions stay on the first line of a recipient list that unfolds.
        QQC2.Label { text: "To:"; opacity: 0.8; Layout.alignment: Qt.AlignTop }
        ExpandableValue {
            id: toLabel
            // Lines the caret up under the date: the From row and this one are
            // the same grid column, so the date's x is the same offset here.
            caretX: dateLabel.x
            text: viewer.context ? viewer.context.to : ""
        }

        QQC2.Label {
            text: "Cc:"
            opacity: 0.8
            Layout.alignment: Qt.AlignTop
            visible: ccLabel.text.length > 0
        }
        ExpandableValue {
            id: ccLabel
            caretX: dateLabel.x
            visible: text.length > 0
            text: viewer.context ? viewer.context.cc : ""
        }

        QQC2.Label { text: "Subject:"; opacity: 0.8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            SelectableValue {
                id: subjectLabel
                font.bold: true
                text: viewer.context
                      ? (viewer.context.subject.length > 0 ? viewer.context.subject
                                                           : (viewer.hasMessage ? "(no subject)" : ""))
                      : ""
            }
            QQC2.ToolButton {
                text: "HTML"
                checkable: true
                checked: viewer.viewMode === "html"
                onClicked: viewer.showMode("html")
            }
            QQC2.ToolButton {
                text: "Text"
                checkable: true
                checked: viewer.viewMode === "text"
                onClicked: viewer.showMode("text")
            }
            QQC2.ToolButton {
                text: "Source"
                checkable: true
                checked: viewer.viewMode === "source"
                onClicked: viewer.showMode("source")
                QQC2.ToolTip.text: "Show the raw message source ("
                                   + (viewer.ui ? viewer.ui.shortcutSource : "Ctrl+U") + ")"
                QQC2.ToolTip.visible: hovered
            }
        }

        Item { visible: authRow.visible } // caption column stays empty
        RowLayout {
            id: authRow
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            visible: dkimLabel.text.length > 0 || arcLabel.text.length > 0
                     || serverAuthLabel.text.length > 0

            // What *we* verified, cryptographically. Deliberately separate from
            // the server's say-so next to it: one is a signature checked against
            // a key we fetched, the other is a header we chose to believe.
            QQC2.Label {
                id: dkimLabel
                visible: text.length > 0
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.bold: viewer.context && viewer.context.dkimStatus === "fail"
                color: {
                    if (!viewer.context || viewer.context.dkimChecking)
                        return Kirigami.Theme.textColor
                    if (viewer.context.dkimTrusted)
                        return Kirigami.Theme.positiveTextColor
                    if (viewer.context.dkimStatus === "fail")
                        return Kirigami.Theme.negativeTextColor
                    if (viewer.context.dkimStatus === "pass")
                        return Kirigami.Theme.neutralTextColor // valid but unaligned
                    return Kirigami.Theme.textColor // "unverified" reads as neutral
                }
                opacity: viewer.context && viewer.context.dkimChecking ? 0.6 : 1
                text: {
                    if (!viewer.context)
                        return ""
                    if (viewer.context.dkimChecking)
                        return "checking signature…"
                    switch (viewer.context.dkimStatus) {
                    case "pass":
                        // "verified" only when the signing domain matches the
                        // sender — a valid signature from some other domain is
                        // exactly what a forgery looks like.
                        return viewer.context.dkimTrusted
                            ? "✓ DKIM verified" : "⚠ DKIM signed by another domain"
                    case "fail":
                        // Covers permanent errors too: an obsolete algorithm or
                        // a revoked key is a signature that cannot be trusted.
                        return "✗ DKIM signature invalid"
                    case "temperror":
                        return "DKIM not checked"
                    case "unsupported":
                        // Neither verified nor broken: obsolete crypto we will
                        // not lend credibility to by checking it.
                        return "⚠ DKIM uses obsolete crypto"
                    case "unverified":
                        // Body hash mismatch. We cannot tell tampering from our
                        // own copy not being byte-exact, so we do not accuse.
                        return "DKIM not verified"
                    default:
                        return "" // no signature at all — say nothing
                    }
                }
                HoverHandler { id: dkimHover }
                HoverToolTip {
                    hover: dkimHover
                    markFailures: true
                    text: viewer.context ? viewer.context.dkimDetail : ""
                }
            }

            // Kept apart from both neighbours on purpose. DKIM says whether the
            // author's own signature holds; this says whether the hops that
            // carried the message left an unbroken trail — which is worth
            // exactly as much as the reader's trust in the domain named in it,
            // so the sealer is always shown rather than reduced to a tick.
            QQC2.Label {
                id: arcLabel
                visible: text.length > 0
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.bold: viewer.context && viewer.context.arcStatus === "fail"
                color: {
                    if (!viewer.context)
                        return Kirigami.Theme.textColor
                    if (viewer.context.arcStatus === "fail")
                        return Kirigami.Theme.negativeTextColor
                    // Never positive-coloured: an intact chain is a claim by a
                    // third party, not verification of the sender.
                    return Kirigami.Theme.textColor
                }
                text: {
                    if (!viewer.context || viewer.context.dkimChecking)
                        return ""
                    const sealer = viewer.context.arcSealer
                    switch (viewer.context.arcStatus) {
                    case "pass":
                        return "ARC intact via " + sealer
                    case "sealsonly":
                        // Seals held, body could not be checked — say the
                        // weaker thing, not the stronger one.
                        return "ARC chain intact via " + sealer
                    case "fail":
                        return "✗ ARC chain broken"
                    case "error":
                        return "ARC not checked"
                    default:
                        return "" // no chain, or never asked
                    }
                }
                HoverHandler { id: arcHover }
                HoverToolTip {
                    hover: arcHover
                    markFailures: true
                    text: viewer.context && viewer.context.arcDetail.length > 0
                        ? "Forwarding hops (ARC):\n" + viewer.context.arcDetail : ""
                }
            }

            QQC2.Label {
                id: serverAuthLabel
                visible: text.length > 0
                Layout.fillWidth: true
                elide: Text.ElideRight
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                text: viewer.context ? viewer.condenseAuth(viewer.context.authInfo) : ""
                HoverHandler { id: serverAuthHover }
                HoverToolTip {
                    hover: serverAuthHover
                    markFailures: true
                    text: viewer.context && viewer.context.authInfo.length > 0
                        ? "Reported by the receiving server:\n" + viewer.context.authInfo : ""
                }
            }
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        visible: viewer.hasMessage && viewer.context.junkTextOnly && viewer.viewMode === "text"
        type: Kirigami.MessageType.Warning
        text: "Spam folder — showing plain text for safety. Click HTML above to render this message anyway."
    }

    Kirigami.Separator {
        Layout.fillWidth: true
        visible: viewer.hasMessage
    }

    // Find in message. Chromium's own find-in-page does the searching and the
    // counting; it works with JavaScript off, since it runs inside the engine
    // rather than in the (untrusted) page.
    RowLayout {
        id: findBar
        Layout.fillWidth: true
        Layout.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing
        visible: false

        QQC2.TextField {
            id: findField
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            placeholderText: "Find in message"
            // Search as you type, from the top of the document each time.
            onTextChanged: viewer.findRun(false)
            Keys.onReturnPressed: event => viewer.findRun(event.modifiers & Qt.ShiftModifier)
            Keys.onEnterPressed: event => viewer.findRun(event.modifiers & Qt.ShiftModifier)
            Keys.onEscapePressed: viewer.closeFind()
        }

        QQC2.Label {
            opacity: 0.8
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            color: (findField.text.length > 0 && viewer.findMatches === 0)
                   ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            text: findField.text.length === 0 ? ""
                : viewer.findMatches === 0 ? "No matches"
                : viewer.findCurrent + " of " + viewer.findMatches
                  + (viewer.findMatches === 1 ? " match" : " matches")
        }

        QQC2.ToolButton {
            icon.name: "go-up"
            enabled: viewer.findMatches > 0
            onClicked: viewer.findRun(true)
            QQC2.ToolTip.text: "Previous match (Shift+F3)"
            QQC2.ToolTip.visible: hovered
        }
        QQC2.ToolButton {
            icon.name: "go-down"
            enabled: viewer.findMatches > 0
            onClicked: viewer.findRun(false)
            QQC2.ToolTip.text: "Next match (F3)"
            QQC2.ToolTip.visible: hovered
        }
        QQC2.ToolButton {
            id: findCase
            text: "Aa"
            checkable: true
            onToggled: viewer.findRun(false)
            QQC2.ToolTip.text: "Match case"
            QQC2.ToolTip.visible: hovered
        }
        QQC2.ToolButton {
            icon.name: "dialog-close"
            onClicked: viewer.closeFind()
            QQC2.ToolTip.text: "Close the find bar (Esc)"
            QQC2.ToolTip.visible: hovered
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: viewer.hasMessage

    WebEngineView {
        id: web
        anchors.fill: parent

        // Hostile-content sandbox: no scripts, no plugins, nothing local.
        // Remote requests are additionally blocked by the C++ interceptor.
        settings.javascriptEnabled: false
        settings.pluginsEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        settings.localStorageEnabled: false
        settings.autoLoadImages: true
        settings.hyperlinkAuditingEnabled: false

        onLoadingChanged: function (loadInfo) {
            if (loadInfo.status === WebEngineView.LoadFailedStatus)
                console.warn("mailo viewer: load failed:", loadInfo.errorString, loadInfo.url)
            // A new document (another message, or the same one switched to
            // Text/Source) has no highlighting and no counts — search it again.
            if (loadInfo.status === WebEngineView.LoadSucceededStatus) {
                viewer.findMatches = 0
                viewer.findCurrent = 0
                if (viewer.findActive)
                    viewer.findRun(false)
            }
        }

        onFindTextFinished: function (result) {
            viewer.findMatches = result.numberOfMatches
            viewer.findCurrent = result.activeMatch
        }

        onNavigationRequested: function (request) {
            // Never navigate inside the viewer; open link clicks externally.
            if (request.navigationType === WebEngineNavigationRequest.LinkClickedNavigation) {
                request.reject()
                Mail.openExternalUrl(request.url)
            }
        }

        // target="_blank" links (most email links) arrive here, not as navigation.
        onNewWindowRequested: function (request) {
            Mail.openExternalUrl(request.requestedUrl)
        }
    }

    // Attachments — overlay bar pinned to the bottom. An overlay (rather than
    // a layout row) so toggling it never resizes the WebEngineView, which
    // repaints with visible glitches on resize.
    Rectangle {
        visible: viewer.context && viewer.context.attachments.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: attachRow.implicitHeight + Kirigami.Units.smallSpacing * 2 + 1
        color: Kirigami.Theme.backgroundColor

        Kirigami.Separator {
            anchors.top: parent.top
            width: parent.width
        }
        Flickable {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.smallSpacing
            anchors.topMargin: Kirigami.Units.smallSpacing + 1
            contentWidth: attachRow.implicitWidth
            clip: true

            Row {
                id: attachRow
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: viewer.context ? viewer.context.attachments : []
                    delegate: QQC2.Button {
                        required property var modelData
                        required property int index
                        icon.name: "mail-attachment"
                        text: modelData.name + " (" + modelData.sizeText + ")"
                        onClicked: { // left click = open (risky types need confirmation)
                            if (viewer.context.attachmentRisky(index)) {
                                confirmOpenDialog.attachmentIndex = index
                                confirmOpenDialog.attachmentName = modelData.name
                                confirmOpenDialog.open()
                            } else {
                                viewer.context.openAttachment(index)
                            }
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton // right click = save to ~/Downloads
                            onTapped: viewer.context.saveAttachmentToDownloads(index)
                        }
                        QQC2.ToolTip.text: "Click to open — right-click to save to Downloads"
                        QQC2.ToolTip.visible: hovered
                    }
                }
            }
        }
    }
    }

    Kirigami.PlaceholderMessage {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !viewer.hasMessage
        text: "Select a message"
        icon.name: "mail-message"
    }

    QQC2.Dialog {
        id: confirmOpenDialog
        property int attachmentIndex: -1
        property string attachmentName: ""
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Open executable attachment?"

        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: "Open anyway"
                icon.name: "dialog-warning"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
            }
            QQC2.Button {
                text: "Cancel"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
            }
        }
        onAccepted: viewer.context.openAttachment(attachmentIndex)

        contentItem: QQC2.Label {
            text: "\"" + confirmOpenDialog.attachmentName + "\" is a script, program "
                  + "or installer. Opening it can run code on this computer.\n\n"
                  + "Only continue if you trust the sender — and remember the "
                  + "sender address itself can be forged."
            wrapMode: Text.Wrap
        }
    }

}
