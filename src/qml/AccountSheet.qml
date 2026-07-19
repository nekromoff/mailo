import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Mailo.Core

QQC2.Dialog {
    id: sheet
    title: "Settings"
    modal: true
    standardButtons: QQC2.Dialog.Save | QQC2.Dialog.Cancel
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 4 : 820, 820)
    // Fixed height so switching between sections doesn't resize the dialog
    height: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 4 : 999,
                     Kirigami.Units.gridUnit * 30)

    /// The window's persisted UI settings object (set by Main.qml).
    property var ui

    /// 0 = Accounts, 1 = General, 2 = Look (UI)
    property int page: 0

    /// Account being edited; -1 = creating a new one.
    property int editIndex: -1

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
    }

    onOpened: {
        editIndex = Mail.accountNames.length > 0 ? Mail.currentAccount : -1
        loadDetails()
    }

    onAccepted: {
        // Look-page settings apply live; Save only persists account edits.
        if (page !== 0)
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
            signature: signatureEdit.text
        })
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

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        // Settings sections
        ColumnLayout {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 8
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
                text: "Look (UI)"
                icon.name: "preferences-desktop-theme"
                highlighted: sheet.page === 2
                onClicked: sheet.page = 2
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
                    model: Mail.accountNames
                    delegate: QQC2.ItemDelegate {
                        required property string modelData
                        required property int index
                        width: accountList.width
                        text: modelData
                        icon.name: "user-identity"
                        highlighted: sheet.editIndex === index
                        onClicked: {
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
                    opacity: 0.6
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
                opacity: 0.6
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
                opacity: 0.6
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }
        } // general ScrollView

        // --- Page 2: Look (UI) ---
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
                opacity: 0.6
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }
        } // look ScrollView
        } // StackLayout
    }
}
