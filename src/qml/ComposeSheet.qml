// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Mailo.Core

Window {
    id: sheet
    title: "Compose"
    flags: Qt.Window
    transientParent: null // own top-level window with its own taskbar entry

    // Last compose geometry, restored on every opening and across restarts.
    // x/y are best effort (Wayland places windows itself); -1 = never saved.
    Settings {
        id: windowState
        category: "composeWindow"
        property int width: 700
        property int height: 600
        property int x: -1
        property int y: -1
        property bool maximized: false
    }
    width: windowState.width
    height: windowState.height
    minimumWidth: 400
    minimumHeight: 300
    Component.onCompleted: {
        if (windowState.x >= 0) {
            x = windowState.x
            y = windowState.y
        }
    }
    function saveGeometry() {
        windowState.maximized = sheet.visibility === Window.Maximized
        // Keep the last windowed geometry — see Main.qml.
        if (sheet.visibility === Window.Windowed) {
            windowState.width = sheet.width
            windowState.height = sheet.height
            windowState.x = sheet.x
            windowState.y = sheet.y
        }
    }
    // Resolved from the content layout's Window color set (chrome gray),
    // so the window fill matches the panel. bgColor override still wins.
    color: panelColor

    /// The uiSettings object from Main.qml (Look settings).
    property var ui: null

    // The panel/chrome follows the Window color set (chrome gray); the
    // colorSet is applied on the content ColumnLayout below (Kirigami.Theme
    // attaches to Items, not the Window). Input fields opt back into the
    // View set (white). A user bgColor override still wins.
    readonly property color panelColor: ui && ui.bgColor !== ""
        ? ui.bgColor : content.Kirigami.Theme.backgroundColor

    property list<url> attachments
    property bool focusBodyOnOpen: false
    // True from the moment Send is triggered until the send resolves. Drives
    // the Send button's "Sending…" state; cleared on failure, and the window
    // closes outright on success.
    property bool sending: false

    // Single send entry point for the button and the Ctrl+Enter shortcut.
    function doSend() {
        if (toField.text.trim().length === 0 || sending)
            return
        sending = true
        Mail.sendMail(toField.text, ccField.text, bccField.text,
                      subjectField.text, bodyEdit.text, attachments)
    }

    /// Set while closing deliberately (sent, draft saved, discard confirmed),
    /// so onClosing lets it through instead of asking again.
    property bool closingConfirmed: false

    function hasContent() {
        return toField.text.trim() !== "" || ccField.text.trim() !== ""
            || bccField.text.trim() !== "" || subjectField.text.trim() !== ""
            || bodyEdit.text.trim() !== "" || attachments.length > 0
    }

    /// The fields exactly as the composer was opened (captured in present()),
    /// so closing can tell "untouched" from "work in progress". A reply's
    /// quoted body or a resumed draft counts as untouched until the user
    /// actually edits something.
    property var openedState: null
    function captureOpenedState() {
        openedState = {
            to: toField.text,
            cc: ccField.text,
            bcc: bccField.text,
            subject: subjectField.text,
            // Read back through the editor: TextArea re-serializes rich text,
            // so comparing against this (not the HTML we assigned) is stable.
            body: bodyEdit.text,
            attachCount: attachments.length
        }
    }
    function isModified() {
        if (!openedState)
            return hasContent()
        return toField.text !== openedState.to
            || ccField.text !== openedState.cc
            || bccField.text !== openedState.bcc
            || subjectField.text !== openedState.subject
            || bodyEdit.text !== openedState.body
            || attachments.length !== openedState.attachCount
    }

    // Escape closes the window, which routes through onClosing below — so an
    // edited message gets the same confirmation as the title bar's X, and an
    // untouched one just closes.
    Shortcut {
        sequence: "Esc"
        onActivated: sheet.close()
    }

    // Closing only asks when there is something to lose: a composer the user
    // has not typed into (empty new message, unedited reply/forward/draft)
    // closes silently — nothing the user wrote is being thrown away, and a
    // resumed draft stays in the Drafts folder untouched.
    onClosing: close => {
        // Remember the geometry even when the discard dialog cancels the
        // close below — a no-op then, the window is still on screen.
        saveGeometry()
        if (closingConfirmed || sending || !isModified())
            return
        close.accepted = false
        discardDialog.open()
    }

    function present() {
        sending = false                  // never reopen stuck in "Sending…"
        closingConfirmed = false
        // Fields are fully populated by the open*() caller at this point —
        // this snapshot is what "unchanged, close silently" compares against.
        captureOpenedState()
        if (windowState.maximized)
            showMaximized()
        else
            show()
        raise()
        requestActivate()
        if (focusBodyOnOpen) {
            bodyEdit.forceActiveFocus()
            bodyEdit.cursorPosition = 0
        } else {
            toField.forceActiveFocus()
        }
    }

    function openNew() {
        sourceDraftUid = -1
        title = "Compose"
        toField.text = ""
        ccField.text = ""
        bccField.text = ""
        subjectField.text = ""
        bodyEdit.text = Mail.newMessageBody() // "" or cursor line + signature
        attachments = []
        content.ccExpanded = false
        focusBodyOnOpen = false
        present()
    }

    /// uid of the Drafts message this composer was opened from, so the stale
    /// copy can be removed once its replacement is stored. -1 for a new message.
    property real sourceDraftUid: -1

    /// d = Mail.draftData(): {to, cc, bcc, subject, body, uid}. Nothing is
    /// quoted or prefixed — the draft is resumed exactly as it was saved.
    function openDraft(d) {
        if (!d || d.subject === undefined)
            return
        title = "Draft"
        toField.text = d.to
        ccField.text = d.cc
        bccField.text = d.bcc
        subjectField.text = d.subject
        bodyEdit.text = d.body
        attachments = []
        sourceDraftUid = d.uid
        content.ccExpanded = d.cc.length > 0 || d.bcc.length > 0
        focusBodyOnOpen = true
        present()
    }

    /// r = Mail.replyData(): {to, cc, subject, body} — empty when no message.
    function openReply(r) {
        if (!r || r.to === undefined)
            return
        sourceDraftUid = -1
        title = "Reply"
        toField.text = r.to
        ccField.text = r.cc
        bccField.text = ""
        subjectField.text = r.subject
        bodyEdit.text = r.body
        attachments = []
        // Reveal Cc/Bcc when a reply pre-fills Cc, so it isn't hidden.
        content.ccExpanded = r.cc.length > 0
        focusBodyOnOpen = true
        present()
    }

    /// r = Mail.forwardData(): {to, cc, subject, body} — empty when no message.
    function openForward(r) {
        if (!r || r.to === undefined)
            return
        sourceDraftUid = -1
        title = "Forward"
        toField.text = r.to
        ccField.text = r.cc
        bccField.text = ""
        subjectField.text = r.subject
        bodyEdit.text = r.body
        attachments = []
        content.ccExpanded = r.cc.length > 0
        focusBodyOnOpen = false // recipient is still to be chosen
        present()
    }

    // Recipient field with autocompletion from previously used addresses.
    // Suggestions match the address token under the cursor; Up/Down pick,
    // Enter/Tab or a click insert, Esc dismisses.
    component AddressField: QQC2.TextField {
        id: addrField

        property var suggestions: []
        property bool suppressCompletion: false

        function refreshSuggestions() {
            if (suppressCompletion || !activeFocus) {
                suggestionPopup.close()
                return
            }
            const token = text.substring(0, cursorPosition).split(",").pop().trim()
            let list = token.length > 0 ? Mail.recipientSuggestions(token) : []
            const present = text.toLowerCase()
            list = list.filter(a => !present.includes(a.toLowerCase()))
            suggestions = list
            if (list.length > 0) {
                suggestionList.currentIndex = 0
                suggestionPopup.open()
            } else {
                suggestionPopup.close()
            }
        }

        function acceptSuggestion() {
            if (!suggestionPopup.visible || suggestionList.currentIndex < 0)
                return false
            const addr = suggestions[suggestionList.currentIndex]
            suppressCompletion = true
            const head = text.substring(0, cursorPosition)
            const tail = text.substring(cursorPosition)
            const cut = head.lastIndexOf(",")
            const newHead = (cut >= 0 ? head.substring(0, cut + 1) + " " : "") + addr
            text = newHead + tail
            cursorPosition = newHead.length
            suppressCompletion = false
            suggestionPopup.close()
            return true
        }

        onTextChanged: refreshSuggestions()
        onActiveFocusChanged: {
            if (!activeFocus)
                suggestionPopup.close()
        }

        Keys.onDownPressed: event => {
            if (suggestionPopup.visible)
                suggestionList.currentIndex =
                    Math.min(suggestionList.currentIndex + 1, suggestions.length - 1)
            else
                event.accepted = false
        }
        Keys.onUpPressed: event => {
            if (suggestionPopup.visible)
                suggestionList.currentIndex = Math.max(suggestionList.currentIndex - 1, 0)
            else
                event.accepted = false
        }
        Keys.onReturnPressed: event => {
            if (!acceptSuggestion())
                event.accepted = false
        }
        Keys.onTabPressed: event => {
            if (!acceptSuggestion())
                event.accepted = false
        }
        Keys.onEscapePressed: event => {
            if (suggestionPopup.visible)
                suggestionPopup.close()
            else
                event.accepted = false
        }

        QQC2.Popup {
            id: suggestionPopup
            y: addrField.height
            width: addrField.width
            padding: 0
            focus: false // keep typing in the field
            closePolicy: QQC2.Popup.CloseOnPressOutsideParent

            contentItem: ListView {
                id: suggestionList
                implicitHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 10)
                clip: true
                model: addrField.suggestions
                delegate: QQC2.ItemDelegate {
                    required property string modelData
                    required property int index
                    width: suggestionList.width
                    text: modelData
                    highlighted: suggestionList.currentIndex === index
                    onHoveredChanged: {
                        if (hovered)
                            suggestionList.currentIndex = index
                    }
                    onClicked: {
                        suggestionList.currentIndex = index
                        addrField.acceptSuggestion()
                    }
                }
            }
        }
    }

    Connections {
        target: Mail
        function onMailSent() {
            if (sheet.visible) {
                // The draft this was resumed from is superseded — without
                // this, sending an edited draft leaves the old one behind.
                Mail.discardDraft(sheet.sourceDraftUid)
                sheet.closingConfirmed = true
                sheet.close()
            }
        }
        function onDraftSaved() {
            if (sheet.visible) {
                // The superseded copy is removed by saveDraft() itself, in
                // sequence with the append — doing it from here raced the
                // refresh and briefly showed both.
                sheet.closingConfirmed = true
                sheet.close()
            }
        }
        // Sending failed: revert the Send button, keep this window open, and
        // show the full server error in a dismissible dialog centered on it.
        function onSendFailed(error) {
            if (!sheet.visible)
                return
            sheet.sending = false
            sendErrorDialog.errorText = error
            sendErrorDialog.open()
        }
    }

    QQC2.Dialog {
        id: discardDialog
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Discard this message?"

        // Discarding a resumed draft removes it outright — not to Trash.
        // Keeping it would mean "Discard" left the message sitting in
        // Drafts, which is the opposite of what it says. A no-op for a
        // message that was never a draft.
        function confirmDiscard() {
            Mail.discardDraft(sheet.sourceDraftUid)
            sheet.closingConfirmed = true
            discardDialog.close()
            sheet.close()
        }

        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: "Discard"
                icon.name: "edit-delete"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.DestructiveRole
                onClicked: discardDialog.confirmDiscard()
            }
            QQC2.Button {
                text: "Save as draft"
                icon.name: "document-save"
                enabled: Mail.hasDraftsFolder
                // No closingConfirmed here: the window closes on draftSaved,
                // once the server has actually taken it. Closing first would
                // throw the message away if the APPEND failed.
                onClicked: {
                    discardDialog.close()
                    Mail.saveDraft(toField.text, ccField.text, bccField.text,
                                   subjectField.text, bodyEdit.text, sheet.attachments,
                                   sheet.sourceDraftUid)
                }
            }
            QQC2.Button {
                text: "Keep editing"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
            }
        }

        contentItem: QQC2.Label {
            // Deleting an existing draft is not the same act as abandoning an
            // unsaved message, so it does not get the same wording.
            text: sheet.sourceDraftUid > 0
                ? "This draft will be deleted from the Drafts folder.\n"
                  + "It does not go to Trash and cannot be recovered."
                : "This message has not been sent."
            wrapMode: Text.Wrap
        }
    }

    QQC2.Dialog {
        id: sendErrorDialog
        property string errorText: ""
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(sheet.width - Kirigami.Units.gridUnit * 4,
                        Kirigami.Units.gridUnit * 30)
        title: "Message not sent"
        standardButtons: QQC2.Dialog.Close

        contentItem: QQC2.Label {
            text: sendErrorDialog.errorText
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
    }

    DocumentHandler {
        id: docHandler
        document: bodyEdit.textDocument
        cursorPosition: bodyEdit.cursorPosition
        selectionStart: bodyEdit.selectionStart
        selectionEnd: bodyEdit.selectionEnd
    }

    FileDialog {
        id: attachDialog
        fileMode: FileDialog.OpenFiles
        // Use the platform (KDE/Breeze) native picker — with the
        // org.kde.desktop Controls style there is no styled QtQuick file
        // dialog, so the native one is the one that matches the app.
        onAccepted: {
            for (const f of selectedFiles)
                sheet.attachments.push(f)
        }
    }

    // Attach shortcut (configurable in Settings → Shortcuts; default Ctrl+Shift+A).
    Shortcut {
        sequence: sheet.ui ? sheet.ui.shortcutAttach : "Ctrl+Shift+A"
        onActivated: attachDialog.open()
    }

    // Send shortcut (configurable; default Ctrl+Return). Also accepts the
    // numeric-keypad Enter alongside the configured sequence, and honours the
    // Send button's guard.
    Shortcut {
        sequences: sheet.ui ? [sheet.ui.shortcutSend, "Ctrl+Enter"]
                            : ["Ctrl+Return", "Ctrl+Enter"]
        enabled: toField.text.trim().length > 0 && !sheet.sending
        onActivated: sheet.doSend()
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.smallSpacing

        // Chrome-gray panel; individual input fields override back to View.
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        Kirigami.Theme.inherit: false

        AddressField {
            id: toField
            Layout.fillWidth: true
            placeholderText: "To (comma-separated)"
            // White field on the gray panel.
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
        }

        // Cc/Bcc are collapsed by default behind a single clickable row.
        property bool ccExpanded: false
        // Left inset the toggle arrow occupies — Bcc aligns to it so its field
        // starts exactly under the Cc field. Measured from the button rather
        // than from the icon size: a ToolButton is its icon *plus* padding, so
        // assuming the icon alone left Bcc short by that padding.
        readonly property real ccArrowInset:
            ccToggleButton.width + Kirigami.Units.smallSpacing

        // Row 1: collapsed → "[>] Cc + Bcc" toggle; expanded → "[⌄] <Cc field>".
        // The arrow stays put on the left in both states and toggles on click.
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            // Focusable toggle: reachable by Tab, and Space/Enter opens or
            // collapses it (standard button-key behaviour).
            QQC2.ToolButton {
                id: ccToggleButton
                icon.name: content.ccExpanded ? "arrow-down" : "arrow-right"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                activeFocusOnTab: true
                onClicked: {
                    content.ccExpanded = !content.ccExpanded
                    // Opening from the keyboard: drop straight into the Cc field.
                    if (content.ccExpanded)
                        ccField.forceActiveFocus()
                }
                QQC2.ToolTip.text: content.ccExpanded ? "Hide Cc/Bcc" : "Show Cc/Bcc"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.Label {
                visible: !content.ccExpanded
                text: "Cc + Bcc"
                opacity: 0.8
                Layout.fillWidth: true
                // Clicking the label is the same as the toggle button.
                TapHandler { onTapped: content.ccExpanded = !content.ccExpanded }
            }
            AddressField {
                id: ccField
                visible: content.ccExpanded
                Layout.fillWidth: true
                placeholderText: "Cc"
                Kirigami.Theme.colorSet: Kirigami.Theme.View
                Kirigami.Theme.inherit: false
            }
        }
        AddressField {
            id: bccField
            visible: content.ccExpanded
            Layout.fillWidth: true
            // Align under the Cc field (past the arrow inset).
            Layout.leftMargin: content.ccArrowInset
            placeholderText: "Bcc"
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
        }
        QQC2.TextField {
            id: subjectField
            Layout.fillWidth: true
            placeholderText: "Subject"
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
        }

        // Formatting toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.ToolButton {
                icon.name: "format-text-bold"
                checkable: true
                checked: docHandler.bold
                onClicked: docHandler.bold = checked
                QQC2.ToolTip.text: "Bold (Ctrl+B)"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "format-text-italic"
                checkable: true
                checked: docHandler.italic
                onClicked: docHandler.italic = checked
                QQC2.ToolTip.text: "Italic (Ctrl+I)"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.SpinBox {
                from: 6
                to: 48
                value: docHandler.fontSize
                onValueModified: docHandler.fontSize = value
                QQC2.ToolTip.text: "Font size"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "format-list-unordered"
                onClicked: docHandler.toggleBulletList()
                QQC2.ToolTip.text: "Bulleted list"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "format-list-ordered"
                onClicked: docHandler.toggleOrderedList()
                QQC2.ToolTip.text: "Numbered list"
                QQC2.ToolTip.visible: hovered
            }
            Item { Layout.fillWidth: true }
            QQC2.ToolButton {
                icon.name: "mail-attachment"
                text: "Attach"
                onClicked: attachDialog.open()
                QQC2.ToolTip.text: "Attach a file (" +
                    (sheet.ui ? sheet.ui.shortcutAttach : "Ctrl+Shift+A") + ")"
                QQC2.ToolTip.visible: hovered
            }
        }

        // Attachment chips
        Flow {
            Layout.fillWidth: true
            visible: sheet.attachments.length > 0
            spacing: Kirigami.Units.smallSpacing
            Repeater {
                model: sheet.attachments
                delegate: QQC2.Button {
                    required property url modelData
                    required property int index
                    icon.name: "edit-delete-remove"
                    text: modelData.toString().split("/").pop()
                    onClicked: {
                        const copy = sheet.attachments
                        copy.splice(index, 1)
                        sheet.attachments = copy
                    }
                    QQC2.ToolTip.text: "Remove attachment"
                    QQC2.ToolTip.visible: hovered
                }
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // The editing area is a white View surface on the gray panel.
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            QQC2.TextArea {
                id: bodyEdit
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                persistentSelection: true
                // White editing surface (View set) on the gray panel, like the
                // recipient/subject fields. Let the style draw its own text and
                // background under the View set — matching the TextFields above
                // — rather than hand-painting them.
                Kirigami.Theme.colorSet: Kirigami.Theme.View
                Kirigami.Theme.inherit: false

                // Standard formatting shortcuts (Ctrl+B / Ctrl+I) toggle the
                // toolbar's bold/italic on the current selection.
                Keys.onPressed: event => {
                    if (event.modifiers & Qt.ControlModifier) {
                        if (event.key === Qt.Key_B) {
                            docHandler.bold = !docHandler.bold
                            event.accepted = true
                        } else if (event.key === Qt.Key_I) {
                            docHandler.italic = !docHandler.italic
                            event.accepted = true
                        }
                    }
                }
            }
        }

        // Discard on the left; a spacer pushes Send to the right, where it
        // occupies exactly half the row width (the primary action).
        RowLayout {
            id: buttonRow
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                id: saveDraftButton
                // Discarding now lives on the window's close button, which
                // closed without asking anyway — this slot is worth more as
                // the action that keeps the message.
                text: "Save as draft"
                icon.name: "document-save"
                enabled: Mail.hasDraftsFolder && !sheet.sending
                QQC2.ToolTip.text: Mail.hasDraftsFolder
                    ? "Store this message in the Drafts folder"
                    : "No Drafts folder on this account"
                QQC2.ToolTip.visible: hovered
                onClicked: Mail.saveDraft(toField.text, ccField.text, bccField.text,
                                          subjectField.text, bodyEdit.text, sheet.attachments,
                                          sheet.sourceDraftUid)
            }
            Item { Layout.fillWidth: true } // spacer takes the remaining left space

            // Right half: the Send button, or — while sending — a spinner and
            // "Sending…" label in its place (the button is hidden, not greyed).
            Item {
                Layout.preferredWidth: (buttonRow.width - buttonRow.spacing * 2) / 2
                Layout.preferredHeight: sendButton.implicitHeight

                QQC2.Button {
                    id: sendButton
                    anchors.fill: parent
                    visible: !sheet.sending
                    text: "Send"
                    icon.name: "document-send"
                    enabled: toField.text.trim().length > 0
                    onClicked: sheet.doSend()
                    QQC2.ToolTip.text: "Send (" +
                        (sheet.ui ? sheet.ui.shortcutSend : "Ctrl+Return") + ")"
                    QQC2.ToolTip.visible: hovered
                }

                RowLayout {
                    anchors.centerIn: parent
                    visible: sheet.sending
                    spacing: Kirigami.Units.smallSpacing

                    // Plain blue arc spinner (same as the main window) — the
                    // desktop-style BusyIndicator draws a cogwheel.
                    Item {
                        id: sendSpinner
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium

                        Canvas {
                            anchors.fill: parent
                            anchors.margins: 2
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                const w = width / 2
                                ctx.beginPath()
                                ctx.arc(w, height / 2, w - 1.5, 0, Math.PI * 1.5)
                                ctx.strokeStyle = Kirigami.Theme.highlightColor
                                ctx.lineWidth = 3
                                ctx.lineCap = "round"
                                ctx.stroke()
                            }
                            RotationAnimator on rotation {
                                running: sheet.sending
                                from: 0
                                to: 360
                                duration: 900
                                loops: Animation.Infinite
                            }
                        }
                    }
                    QQC2.Label { text: "Sending…" }
                }
            }
        }
    }
}
