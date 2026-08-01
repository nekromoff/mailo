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
    title: "Settings"
    // A real top-level window: the system title bar provides close, minimize
    // and maximize, the window manager handles resizing (the old hand-rolled
    // corner grip is gone), and it gets its own taskbar entry.
    flags: Qt.Window
    transientParent: null
    // No Cancel: everything outside the Accounts page applies live, so the
    // button only ever discarded account edits while silently keeping the
    // rest. Escape (and the title bar's close) dismisses without saving.
    // The last geometry, remembered across restarts. x/y are best effort
    // (Wayland places windows itself); -1 = never saved, let the WM place it.
    Settings {
        id: dialogSettings
        category: "settingsDialog"
        property real width: 820
        property real height: Kirigami.Units.gridUnit * 30
        property int x: -1
        property int y: -1
        property bool maximized: false
    }
    width: dialogSettings.width
    height: dialogSettings.height
    minimumWidth: 560
    minimumHeight: 360
    Component.onCompleted: {
        if (dialogSettings.x >= 0) {
            x = dialogSettings.x
            y = dialogSettings.y
        }
    }
    onClosing: {
        dialogSettings.maximized = sheet.visibility === Window.Maximized
        // Keep the last windowed geometry — see Main.qml.
        if (sheet.visibility === Window.Windowed) {
            dialogSettings.width = sheet.width
            dialogSettings.height = sheet.height
            dialogSettings.x = sheet.x
            dialogSettings.y = sheet.y
        }
    }

    // Chrome-gray panel (Window color set), same treatment as the compose
    // window; the user's bgColor override wins.
    color: ui && ui.bgColor !== "" ? ui.bgColor
                                   : content.Kirigami.Theme.backgroundColor

    /// The window's persisted UI settings object (set by Main.qml).
    property var ui

    /// 0 = Accounts, 1 = General, 2 = Look and feel, 3 = Shortcuts
    property int page: 0

    /// True while a shortcut-capture button is reading raw key presses — the
    /// Esc shortcut below stands down so Esc can cancel the capture instead
    /// of closing the window.
    property bool captureActive: false

    /// Shows the window (the Dialog-era entry point, kept so callers and the
    /// old open() semantics — reload the account form on every opening —
    /// stay unchanged).
    function open() {
        editIndex = Mail.accountNames.length > 0 ? Mail.currentAccount : -1
        loadDetails()
        if (dialogSettings.maximized)
            showMaximized()
        else
            show()
        raise()
        requestActivate()
    }

    Shortcut {
        sequence: "Esc"
        enabled: !sheet.captureActive
        onActivated: sheet.close()
    }

    /// Account being edited; -1 = creating a new one.
    property int editIndex: -1

    /// OAuth providers supply their own servers, so only the address matters.
    readonly property bool oauthAccount: authBox.currentIndex !== 0

    /// What the account still needs before it can be saved, or "" when ready.
    /// Saving a half-filled account produced one that could never connect and
    /// had to be deleted and redone, so the button stays off until it would
    /// actually work.
    readonly property string detailsMissing: {
        if (userField.text.trim() === "")
            return oauthAccount ? "an e-mail address" : "a username"
        if (!oauthAccount) {
            if (hostField.text.trim() === "")
                return "an IMAP server"
            if (smtpHostField.text.trim() === "")
                return "an SMTP server for sending"
            // Only for a new account: editing an existing one leaves the field
            // blank on purpose, and the saved password stays as it is.
            if (editIndex < 0 && passwordField.text === "")
                return "a password"
        }
        return ""
    }

    function loadDetails() {
        const d = Mail.accountDetails(editIndex)
        hostField.text = d.host ?? ""
        portField.value = d.port ?? 993
        securityBox.currentIndex = d.security ?? 0
        userField.text = d.user ?? ""
        passwordField.text = ""
        smtpHostField.text = d.smtpHost ?? ""
        smtpPortField.value = d.smtpPort ?? 587
        smtpSecurityBox.currentIndex = d.smtpSecurity ?? 1
        authBox.currentIndex = d.authType ?? 0
        signatureEdit.text = d.signature ?? ""
        htmlMailBox.checked = d.htmlMail ?? true
    }

    /// Persists the account form (the Save button). Closes the window on
    /// success — the same flow the Dialog's accept() used to run.
    function saveAccount() {
        // Look-page settings apply live; Save only persists account edits.
        if (page !== 0)
            return
        // Belt and braces: the button is disabled, but Enter reaches here
        // without going through it.
        if (detailsMissing !== "")
            return
        // OAuth providers get fixed, known-good server settings.
        const presets = authBox.currentIndex === 1
            ? {host: "imap.gmail.com", port: 993, security: 0,
               smtpHost: "smtp.gmail.com", smtpPort: 587, smtpSecurity: 1}
            : authBox.currentIndex === 2
            ? {host: "outlook.office365.com", port: 993, security: 0,
               smtpHost: "smtp.office365.com", smtpPort: 587, smtpSecurity: 1}
            : {host: hostField.text, port: portField.value,
               security: securityBox.currentIndex, smtpHost: smtpHostField.text,
               smtpPort: smtpPortField.value, smtpSecurity: smtpSecurityBox.currentIndex}
        Mail.saveAccountDetails(editIndex, {
            host: presets.host,
            port: presets.port,
            security: presets.security,
            user: userField.text,
            password: passwordField.text,
            savePassword: savePasswordBox.checked,
            smtpHost: presets.smtpHost,
            smtpPort: presets.smtpPort,
            smtpSecurity: presets.smtpSecurity,
            authType: authBox.currentIndex,
            signature: signatureEdit.text,
            htmlMail: htmlMailBox.checked
        })
        close()
    }

    QQC2.Dialog {
        id: confirmRemoveDialog
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Remove account?"

        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: "Remove account"
                icon.name: "edit-delete"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
            }
            QQC2.Button {
                text: "Cancel"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
            }
        }
        onAccepted: {
            Mail.removeAccount(sheet.editIndex)
            sheet.editIndex = Mail.accountNames.length > 0 ? Mail.currentAccount : -1
            sheet.loadDetails()
        }

        contentItem: QQC2.Label {
            text: "Remove \"" + (Mail.accountNames[sheet.editIndex] ?? "") + "\" from Mailo?\n\n"
                  + "Its settings and saved password are deleted from this computer. "
                  + "Mail on the server is not touched."
            wrapMode: Text.Wrap
        }
    }

    DocumentHandler {
        id: signatureDocHandler
        document: signatureEdit.textDocument
        cursorPosition: signatureEdit.cursorPosition
        selectionStart: signatureEdit.selectionStart
        selectionEnd: signatureEdit.selectionEnd
    }

    FileDialog {
        id: signatureImportDialog
        nameFilters: ["HTML files (*.html *.htm)", "All files (*)"]
        onAccepted: {
            const html = Mail.loadHtmlFile(selectedFile)
            if (html.length > 0)
                signatureEdit.text = html
        }
    }

    ColorDialog {
        id: bgColorDialog
        onAccepted: {
            sheet.ui.bgColor = selectedColor.toString()
            bgColorField.text = sheet.ui.bgColor
        }
    }

    ColorDialog {
        id: scaleColorDialog
        property int scaleIndex: 0
        onAccepted: sheet.ui["scaleColor" + scaleIndex] = selectedColor.toString()
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.smallSpacing

        // Chrome-gray panel (dialog-like); the style still draws the pages'
        // input fields with their own backgrounds.
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        Kirigami.Theme.inherit: false

        RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: Kirigami.Units.largeSpacing

        // Settings sections
        ColumnLayout {
            id: sectionList
            // Never narrower than its widest entry: a fixed 8 gridUnits cut
            // off "Look and feel". implicitWidth here is the widest delegate's
            // (icon + text + padding), so the column tracks the labels rather
            // than a guess at how long they are.
            Layout.minimumWidth: implicitWidth
            Layout.preferredWidth: Math.max(implicitWidth, Kirigami.Units.gridUnit * 8)
            Layout.alignment: Qt.AlignTop
            spacing: 0

            QQC2.ItemDelegate {
                Layout.fillWidth: true
                text: "Accounts"
                icon.name: "user-identity"
                highlighted: sheet.page === 0
                onClicked: sheet.page = 0
            }
            QQC2.ItemDelegate {
                Layout.fillWidth: true
                text: "General"
                icon.name: "configure"
                highlighted: sheet.page === 1
                onClicked: sheet.page = 1
            }
            QQC2.ItemDelegate {
                Layout.fillWidth: true
                text: "Look and feel"
                icon.name: "preferences-desktop-theme"
                highlighted: sheet.page === 2
                onClicked: sheet.page = 2
            }
            QQC2.ItemDelegate {
                Layout.fillWidth: true
                text: "Shortcuts"
                icon.name: "input-keyboard"
                highlighted: sheet.page === 3
                onClicked: sheet.page = 3
            }
            QQC2.ItemDelegate {
                Layout.fillWidth: true
                text: "About"
                icon.name: "help-about"
                highlighted: sheet.page === 4
                onClicked: sheet.page = 4
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sheet.page

        // --- Page 0: Accounts ---
        RowLayout {
        spacing: Kirigami.Units.largeSpacing

        // Account list
        ColumnLayout {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 11
            Layout.fillHeight: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ListView {
                    id: accountList
                    // An account being created gets a row of its own straight
                    // away, so it is visible where it will end up instead of
                    // existing only in the form. It follows the username as it
                    // is typed, and is not a real account until Save.
                    readonly property string draftName:
                        userField.text !== "" ? userField.text : "New account"
                    model: sheet.editIndex === -1
                           ? Mail.accountNames.concat([draftName])
                           : Mail.accountNames

                    delegate: QQC2.ItemDelegate {
                        required property string modelData
                        required property int index
                        readonly property bool isDraft: index >= Mail.accountNames.length
                        width: accountList.width
                        text: modelData
                        icon.name: isDraft ? "list-add" : "user-identity"
                        font.italic: isDraft
                        highlighted: isDraft ? sheet.editIndex === -1
                                             : sheet.editIndex === index
                        onClicked: {
                            if (isDraft) {
                                // Already editing it — reloading would wipe
                                // whatever has been typed so far.
                                sheet.editIndex = -1
                                return
                            }
                            sheet.editIndex = index
                            sheet.loadDetails()
                        }
                    }
                }
            }
            QQC2.Button {
                Layout.fillWidth: true
                icon.name: "list-add"
                text: "New account"
                highlighted: sheet.editIndex === -1
                onClicked: {
                    sheet.editIndex = -1
                    sheet.loadDetails()
                }
            }
            QQC2.Button {
                Layout.fillWidth: true
                icon.name: "list-remove"
                text: "Remove"
                enabled: sheet.editIndex >= 0 && Mail.accountNames.length > 0
                onClicked: confirmRemoveDialog.open()
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        // Per-account details — scrolls inside the fixed-height dialog
        QQC2.ScrollView {
            id: detailsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

        Kirigami.FormLayout {
            width: detailsScroll.availableWidth

            Kirigami.Separator {
                Kirigami.FormData.label: sheet.editIndex === -1
                    ? "New account" : "Account: " + (Mail.accountNames[sheet.editIndex] ?? "")
                Kirigami.FormData.isSection: true
            }
            // Account type on top: standard IMAP or an OAuth provider.
            QQC2.ComboBox {
                id: authBox
                Kirigami.FormData.label: "Account type:"
                model: ["Standard (IMAP)", "Gmail", "Microsoft 365"]
            }
            QQC2.TextField {
                id: userField
                Kirigami.FormData.label: authBox.currentIndex === 0 ? "Username:" : "E-mail:"
                placeholderText: "user@example.com"
            }
            QQC2.Label {
                visible: authBox.currentIndex > 0
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Server settings are set up automatically. When you save, "
                      + "your browser opens to sign in — Mailo picks up the "
                      + "sign-in automatically and remembers it in the system keyring."
                wrapMode: Text.Wrap
                opacity: 0.7
            }

            QQC2.TextField {
                id: hostField
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "Server:"
                placeholderText: "imap.example.com"
            }
            QQC2.SpinBox {
                id: portField
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "Port:"
                from: 1
                to: 65535
                value: 993
                editable: true
            }
            QQC2.ComboBox {
                id: securityBox
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "Security:"
                model: ["SSL/TLS", "STARTTLS", "None"]
                onActivated: portField.value = currentIndex === 0 ? 993 : 143
            }
            QQC2.TextField {
                id: passwordField
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "Password:"
                echoMode: TextInput.Password
                placeholderText: sheet.editIndex >= 0 ? "(unchanged)" : ""
            }
            QQC2.CheckBox {
                id: savePasswordBox
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: ""
                text: "Remember password (stored in KWallet / system keyring)"
                checked: true
            }

            Kirigami.Separator {
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "Sending (SMTP)"
                Kirigami.FormData.isSection: true
            }
            QQC2.TextField {
                id: smtpHostField
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "SMTP server:"
                placeholderText: "smtp.example.com"
            }
            QQC2.SpinBox {
                id: smtpPortField
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "SMTP port:"
                from: 1
                to: 65535
                value: 587
                editable: true
            }
            QQC2.ComboBox {
                id: smtpSecurityBox
                visible: authBox.currentIndex === 0
                Kirigami.FormData.label: "SMTP security:"
                model: ["SSL/TLS", "STARTTLS", "None"]
                currentIndex: 1
                onActivated: smtpPortField.value = currentIndex === 0 ? 465 : 587
            }

            Kirigami.Separator {
                Kirigami.FormData.label: "Composing"
                Kirigami.FormData.isSection: true
            }
            QQC2.CheckBox {
                id: htmlMailBox
                Kirigami.FormData.label: "Message format:"
                text: "Send HTML mail"
                checked: true
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "HTML mail carries a plain-text version alongside, so every "
                      + "recipient can read it. Untick to send plain text only."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

            Kirigami.Separator {
                Kirigami.FormData.label: "Signature"
                Kirigami.FormData.isSection: true
            }
            ColumnLayout {
                Kirigami.FormData.label: ""
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                // Formatting toolbar for the rich-text signature
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.ToolButton {
                        icon.name: "format-text-bold"
                        checkable: true
                        checked: signatureDocHandler.bold
                        onClicked: signatureDocHandler.bold = checked
                    }
                    QQC2.ToolButton {
                        icon.name: "format-text-italic"
                        checkable: true
                        checked: signatureDocHandler.italic
                        onClicked: signatureDocHandler.italic = checked
                    }
                    QQC2.SpinBox {
                        from: 6
                        to: 48
                        value: signatureDocHandler.fontSize
                        onValueModified: signatureDocHandler.fontSize = value
                        QQC2.ToolTip.text: "Font size"
                        QQC2.ToolTip.visible: hovered
                    }
                    Item { Layout.fillWidth: true }
                    QQC2.ToolButton {
                        icon.name: "document-import"
                        text: "Import HTML…"
                        onClicked: signatureImportDialog.open()
                        QQC2.ToolTip.text: "Replace the signature with the contents of an HTML file"
                        QQC2.ToolTip.visible: hovered
                    }
                }

                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 7
                    clip: true
                    QQC2.TextArea {
                        id: signatureEdit
                        textFormat: TextEdit.RichText
                        wrapMode: TextEdit.Wrap
                        persistentSelection: true
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: "Added automatically to every new message, reply and "
                          + "forward from this account — above the quoted mail."
                    wrapMode: Text.Wrap
                    opacity: 0.8
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                }
            }
        }
        } // details ScrollView
        } // page 0

        // --- Page 1: General ---
        QQC2.ScrollView {
            id: generalScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

        Kirigami.FormLayout {
            width: generalScroll.availableWidth

            Kirigami.Separator {
                Kirigami.FormData.label: "Mail checking"
                Kirigami.FormData.isSection: true
            }
            QQC2.TextField {
                id: refreshField
                Kirigami.FormData.label: "Refresh every (minutes):"
                implicitWidth: Kirigami.Units.gridUnit * 4
                text: Mail.refreshMinutes
                validator: IntValidator { bottom: 0; top: 1440 }
                onTextEdited: {
                    if (acceptableInput)
                        Mail.refreshMinutes = parseInt(text)
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Checks the open folder for new mail on this schedule when "
                      + "real-time push (IMAP IDLE) is not active. 0 turns it off."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

            Kirigami.Separator {
                Kirigami.FormData.label: "Dates"
                Kirigami.FormData.isSection: true
            }
            QQC2.ComboBox {
                id: dateFormatBox
                Kirigami.FormData.label: "Date format:"
                // Display labels ↔ Qt format strings, index-matched.
                model: ["DD/MM/YYYY", "DD.MM.YYYY", "DD-MM-YYYY",
                        "MM/DD/YYYY", "YYYY-MM-DD"]
                readonly property var formats: ["dd/MM/yyyy", "dd.MM.yyyy", "dd-MM-yyyy",
                                                "MM/dd/yyyy", "yyyy-MM-dd"]
                currentIndex: Math.max(0, formats.indexOf(Mail.dateFormat))
                onActivated: Mail.dateFormat = formats[currentIndex]
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Used for message dates in the list and the reading pane. "
                      + "Messages from today show only their time."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

            Kirigami.Separator {
                Kirigami.FormData.label: "Storage"
                Kirigami.FormData.isSection: true
            }
            QQC2.TextField {
                id: maxBodyField
                Kirigami.FormData.label: "Don't cache messages over (MB):"
                implicitWidth: Kirigami.Units.gridUnit * 4
                text: Mail.maxBodyMB
                validator: IntValidator { bottom: 0; top: 1024 }
                onTextEdited: {
                    if (acceptableInput)
                        Mail.maxBodyMB = parseInt(text)
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Messages larger than this are still opened normally, they "
                      + "are just never stored for offline use — a few big "
                      + "attachments can otherwise outweigh thousands of normal "
                      + "messages. 0 caches everything. Raising the limit makes "
                      + "previously skipped messages eligible again."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
            RowLayout {
                Kirigami.FormData.label: "Offline cache:"
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    id: cacheSizeLabel
                    // Re-read on open rather than polling: the figure only
                    // moves when a purge or a vacuum has run.
                    text: Mail.cacheSizeText()
                    // Read at the same moments as the text, so the button
                    // below agrees with the figure beside it.
                    property bool worthwhile: Mail.reclaimWorthwhile()
                }
                QQC2.BusyIndicator {
                    running: Mail.reclaiming
                    visible: running
                    implicitWidth: Kirigami.Units.gridUnit
                    implicitHeight: Kirigami.Units.gridUnit
                }
            }
            QQC2.Button {
                Kirigami.FormData.label: ""
                text: cacheSizeLabel.worthwhile ? "Reclaim disk space"
                                               : "Nothing to reclaim"
                enabled: !Mail.reclaiming && cacheSizeLabel.worthwhile
                // Close Settings first: the progress dialog is modal over the
                // main window, and leaving this one open would stack two modals.
                onClicked: {
                    sheet.close()
                    Mail.reclaimDiskSpace()
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Deleting cached mail frees space inside the cache file but "
                      + "does not shrink it. This rebuilds the file to give that "
                      + "space back to the disk. It takes several minutes on a "
                      + "large cache and pauses syncing while it runs."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
            Connections {
                target: Mail
                function onReclaimingChanged() {
                    if (!Mail.reclaiming) {
                        cacheSizeLabel.text = Mail.cacheSizeText()
                        cacheSizeLabel.worthwhile = Mail.reclaimWorthwhile()
                    }
                }
            }

            // Last on the page: troubleshooting, not something anyone sets on
            // the way to somewhere else.
            Kirigami.Separator {
                Kirigami.FormData.label: "Diagnostics"
                Kirigami.FormData.isSection: true
            }
            QQC2.CheckBox {
                Kirigami.FormData.label: "Log activity to console:"
                checked: Mail.debugLogging
                onToggled: Mail.debugLogging = checked
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Prints folder, account and sync activity to the terminal "
                      + "mailo was started from. Useful when reporting a bug; "
                      + "takes effect immediately, no restart needed."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }
        } // general ScrollView

        // --- Page 2: Look and feel ---
        QQC2.ScrollView {
            id: lookScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

        Kirigami.FormLayout {
            width: lookScroll.availableWidth

            QQC2.ComboBox {
                Kirigami.FormData.label: "Row size:"
                model: ["Compact", "Medium", "Wide"]
                currentIndex: sheet.ui ? sheet.ui.rowDensity : 1
                onActivated: sheet.ui.rowDensity = currentIndex
            }

            RowLayout {
                Kirigami.FormData.label: "Background color:"
                spacing: Kirigami.Units.smallSpacing

                QQC2.TextField {
                    id: bgColorField
                    implicitWidth: Kirigami.Units.gridUnit * 7
                    text: sheet.ui ? sheet.ui.bgColor : ""
                    placeholderText: "#rrggbb"
                    // Apply live while typing — a Save-button click would
                    // close the dialog before editingFinished ever fired.
                    onTextEdited: {
                        if (text === "" || /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(text))
                            sheet.ui.bgColor = text
                    }
                }
                Rectangle { // swatch
                    width: Kirigami.Units.gridUnit * 1.2
                    height: width
                    radius: 3
                    color: sheet.ui && sheet.ui.bgColor !== ""
                           ? sheet.ui.bgColor : Kirigami.Theme.backgroundColor
                    border.color: Kirigami.Theme.textColor
                    border.width: 1
                }
                QQC2.Button {
                    text: "Pick…"
                    icon.name: "color-picker"
                    onClicked: {
                        bgColorDialog.selectedColor = sheet.ui.bgColor !== ""
                            ? sheet.ui.bgColor : Kirigami.Theme.backgroundColor
                        bgColorDialog.open()
                    }
                }
                QQC2.Button {
                    icon.name: "edit-clear"
                    QQC2.ToolTip.text: "Reset to theme default"
                    QQC2.ToolTip.visible: hovered
                    onClicked: {
                        sheet.ui.bgColor = ""
                        bgColorField.text = ""
                    }
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                text: "Applies to the interface panels. Changes take effect immediately."
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

            Kirigami.Separator {
                Kirigami.FormData.label: "Mark emails"
                Kirigami.FormData.isSection: true
            }
            Repeater {
                model: [1, 2, 3, 4, 5]
                RowLayout {
                    id: scaleRow
                    required property int modelData
                    readonly property string keyProp: "scaleKey" + modelData
                    readonly property string colorProp: "scaleColor" + modelData
                    Kirigami.FormData.label: "Scale " + modelData + ":"
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        id: scaleCapture
                        property bool capturing: false
                        // Hold the window's Esc shortcut off while capturing,
                        // so Esc cancels the capture instead of closing.
                        onCapturingChanged: sheet.captureActive = capturing
                        implicitWidth: Kirigami.Units.gridUnit * 8
                        text: capturing ? "Press keys…"
                                        : (sheet.ui && sheet.ui[scaleRow.keyProp] !== ""
                                           ? sheet.ui[scaleRow.keyProp] : "None")
                        icon.name: capturing ? "input-keyboard" : ""
                        onClicked: {
                            capturing = true
                            forceActiveFocus()
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus)
                                capturing = false
                        }
                        Keys.onPressed: event => {
                            if (!capturing)
                                return
                            event.accepted = true
                            if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift
                                    || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta)
                                return
                            if (event.key === Qt.Key_Escape) {
                                capturing = false
                                return
                            }
                            const seq = shortcutsForm.sequenceFromEvent(event)
                            if (seq !== "") {
                                sheet.ui[scaleRow.keyProp] = seq
                                capturing = false
                            }
                        }
                    }
                    QQC2.Button {
                        icon.name: "edit-clear"
                        enabled: sheet.ui && sheet.ui[scaleRow.keyProp] !== ""
                        QQC2.ToolTip.text: "Clear shortcut"
                        QQC2.ToolTip.visible: hovered
                        onClicked: sheet.ui[scaleRow.keyProp] = ""
                    }
                    Rectangle { // swatch; hatched look when undefined
                        width: Kirigami.Units.gridUnit * 1.2
                        height: width
                        radius: 3
                        color: sheet.ui && sheet.ui[scaleRow.colorProp] !== ""
                               ? sheet.ui[scaleRow.colorProp] : "transparent"
                        border.color: Kirigami.Theme.textColor
                        border.width: 1
                        QQC2.Label {
                            anchors.centerIn: parent
                            visible: !sheet.ui || sheet.ui[scaleRow.colorProp] === ""
                            text: "?"
                            opacity: 0.8
                        }
                    }
                    QQC2.Button {
                        text: "Pick…"
                        icon.name: "color-picker"
                        onClicked: {
                            scaleColorDialog.scaleIndex = scaleRow.modelData
                            scaleColorDialog.selectedColor =
                                sheet.ui[scaleRow.colorProp] !== ""
                                    ? sheet.ui[scaleRow.colorProp]
                                    : Kirigami.Theme.textColor
                            scaleColorDialog.open()
                        }
                    }
                    QQC2.Button {
                        icon.name: "edit-clear"
                        enabled: sheet.ui && sheet.ui[scaleRow.colorProp] !== ""
                        QQC2.ToolTip.text: "Clear color"
                        QQC2.ToolTip.visible: hovered
                        onClicked: sheet.ui[scaleRow.colorProp] = ""
                    }
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Pressing a scale shortcut marks the selected messages "
                      + "with that color (press again to clear the mark). "
                      + "Defined colors appear next to the search bar as a "
                      + "quick filter."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }
        } // look ScrollView

        // --- Page 3: Shortcuts ---
        QQC2.ScrollView {
            id: shortcutsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

        Kirigami.FormLayout {
            id: shortcutsForm
            width: shortcutsScroll.availableWidth

            /// Human/QKeySequence-style string for a captured key press,
            /// or "" when the pressed key cannot stand alone as a shortcut.
            function sequenceFromEvent(event) {
                let s = ""
                if (event.modifiers & Qt.ControlModifier) s += "Ctrl+"
                if (event.modifiers & Qt.AltModifier) s += "Alt+"
                if (event.modifiers & Qt.ShiftModifier) s += "Shift+"
                if (event.modifiers & Qt.MetaModifier) s += "Meta+"
                const named = {}
                named[Qt.Key_Delete] = "Del"
                named[Qt.Key_Backspace] = "Backspace"
                named[Qt.Key_Space] = "Space"
                named[Qt.Key_Insert] = "Ins"
                named[Qt.Key_Home] = "Home"
                named[Qt.Key_End] = "End"
                if (event.key in named)
                    return s + named[event.key]
                if (event.key >= Qt.Key_F1 && event.key <= Qt.Key_F12)
                    return s + "F" + (event.key - Qt.Key_F1 + 1)
                if ((event.key >= Qt.Key_A && event.key <= Qt.Key_Z)
                        || (event.key >= Qt.Key_0 && event.key <= Qt.Key_9))
                    return s + String.fromCharCode(event.key)
                return ""
            }

            Repeater {
                model: [
                    {label: "Select message:", key: "shortcutSelect", def: "Ins"},
                    {label: "Delete message:", key: "shortcutDelete", def: "Del"},
                    {label: "Classify as junk:", key: "shortcutJunk", def: "J"},
                    {label: "Compose:", key: "shortcutCompose", def: "C"},
                    {label: "Reply:", key: "shortcutReply", def: "R"},
                    {label: "Forward:", key: "shortcutForward", def: "F"},
                    {label: "Attach file:", key: "shortcutAttach", def: "Ctrl+Shift+A"},
                    {label: "Send message:", key: "shortcutSend", def: "Ctrl+Return"},
                    {label: "Find in message:", key: "shortcutFind", def: "Ctrl+F"},
                    {label: "View source:", key: "shortcutSource", def: "Ctrl+U"}
                ]
                RowLayout {
                    required property var modelData
                    Kirigami.FormData.label: modelData.label
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        id: captureButton
                        property bool capturing: false
                        // See scaleCapture: keeps Esc for the capture.
                        onCapturingChanged: sheet.captureActive = capturing
                        implicitWidth: Kirigami.Units.gridUnit * 8
                        text: capturing ? "Press keys…"
                                        : (sheet.ui ? sheet.ui[modelData.key]
                                                    : modelData.def)
                        icon.name: capturing ? "input-keyboard" : ""
                        onClicked: {
                            capturing = true
                            forceActiveFocus()
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus)
                                capturing = false
                        }
                        Keys.onPressed: event => {
                            if (!capturing)
                                return
                            event.accepted = true
                            // Wait for a real key — a held modifier is not
                            // a shortcut on its own.
                            if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift
                                    || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta)
                                return
                            if (event.key === Qt.Key_Escape) {
                                capturing = false
                                return
                            }
                            const seq = shortcutsForm.sequenceFromEvent(event)
                            if (seq !== "") {
                                sheet.ui[modelData.key] = seq
                                capturing = false
                            }
                        }
                    }
                    QQC2.Button {
                        icon.name: "edit-clear"
                        QQC2.ToolTip.text: "Reset to default (" + modelData.def + ")"
                        QQC2.ToolTip.visible: hovered
                        onClicked: sheet.ui[modelData.key] = modelData.def
                    }
                }
            }
            QQC2.Label {
                Kirigami.FormData.label: ""
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                text: "Click a shortcut, then press the new key or combination "
                      + "(Esc cancels). Shortcuts act in the mail and folder "
                      + "lists and apply immediately."
                wrapMode: Text.Wrap
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

        }
        } // shortcuts ScrollView

        // --- Page 4: About ---
        QQC2.ScrollView {
            id: aboutScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            // A FormLayout purely for its section heading, so the page title
            // is styled exactly like "Mail checking" or "Storage" rather than
            // being a Markdown heading inside the text.
            Kirigami.FormLayout {
                width: aboutScroll.availableWidth

                Kirigami.Separator {
                    // Same source as the main-bar version label: whatever the
                    // binary was built with.
                    Kirigami.FormData.label: "Mailo v" + Qt.application.version
                    Kirigami.FormData.isSection: true
                }
                QQC2.Label {
                    Kirigami.FormData.label: ""
                    Layout.fillWidth: true
                    // Compiled into the binary from ABOUT.md at build time.
                    text: Mail.aboutText
                    textFormat: Text.MarkdownText
                    wrapMode: Text.Wrap
                    onLinkActivated: link => Mail.openExternalUrl(link)
                }
            }
        } // about ScrollView
        } // StackLayout
        } // content RowLayout

        // Footer: the Save button (and its "what's missing" hint), only on
        // the Accounts page — every other page applies as you change it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            visible: sheet.page === 0

            QQC2.Label {
                visible: sheet.detailsMissing !== ""
                Layout.leftMargin: Kirigami.Units.largeSpacing
                text: "Needs " + sheet.detailsMissing
                opacity: 0.8
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
            Item { Layout.fillWidth: true }
            QQC2.Button {
                text: "Save"
                icon.name: "document-save"
                enabled: sheet.detailsMissing === ""
                onClicked: sheet.saveAccount()
            }
        }
    }
}
