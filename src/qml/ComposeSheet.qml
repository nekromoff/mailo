import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Mailo.Core

QQC2.Dialog {
    id: sheet
    title: "Compose"
    modal: true
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 4 : 700, 700)
    height: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 4 : 600, 600)

    property list<url> attachments
    property bool focusBodyOnOpen: false

    function openNew() {
        title = "Compose"
        toField.text = ""
        ccField.text = ""
        subjectField.text = ""
        bodyEdit.text = Mail.newMessageBody() // "" or cursor line + signature
        attachments = []
        focusBodyOnOpen = false
        open()
    }

    /// r = Mail.replyData(): {to, cc, subject, body} — empty when no message.
    function openReply(r) {
        if (!r || r.to === undefined)
            return
        title = "Reply"
        toField.text = r.to
        ccField.text = r.cc
        subjectField.text = r.subject
        bodyEdit.text = r.body
        attachments = []
        focusBodyOnOpen = true
        open()
    }

    /// r = Mail.forwardData(): {to, cc, subject, body} — empty when no message.
    function openForward(r) {
        if (!r || r.to === undefined)
            return
        title = "Forward"
        toField.text = r.to
        ccField.text = r.cc
        subjectField.text = r.subject
        bodyEdit.text = r.body
        attachments = []
        focusBodyOnOpen = false // recipient is still to be chosen
        open()
    }

    onOpened: {
        if (focusBodyOnOpen) {
            bodyEdit.forceActiveFocus()
            bodyEdit.cursorPosition = 0
        } else {
            toField.forceActiveFocus()
        }
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

    footer: QQC2.DialogButtonBox {
        QQC2.Button {
            text: "Send"
            icon.name: "document-send"
            enabled: toField.text.trim().length > 0 && !Mail.busy
            QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
        }
        QQC2.Button {
            text: "Discard"
            QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
        }
    }
    onAccepted: Mail.sendMail(toField.text, ccField.text, subjectField.text,
                              bodyEdit.text, attachments)

    Connections {
        target: Mail
        function onMailSent() {
            if (sheet.visible)
                sheet.close()
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
        onAccepted: {
            for (const f of selectedFiles)
                sheet.attachments.push(f)
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        AddressField {
            id: toField
            Layout.fillWidth: true
            placeholderText: "To (comma-separated)"
        }
        AddressField {
            id: ccField
            Layout.fillWidth: true
            placeholderText: "Cc"
        }
        QQC2.TextField {
            id: subjectField
            Layout.fillWidth: true
            placeholderText: "Subject"
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
            }
            QQC2.ToolButton {
                icon.name: "format-text-italic"
                checkable: true
                checked: docHandler.italic
                onClicked: docHandler.italic = checked
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
            }
            QQC2.ToolButton {
                icon.name: "format-list-ordered"
                onClicked: docHandler.toggleOrderedList()
            }
            Item { Layout.fillWidth: true }
            QQC2.ToolButton {
                icon.name: "mail-attachment"
                text: "Attach"
                onClicked: attachDialog.open()
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
            QQC2.TextArea {
                id: bodyEdit
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                persistentSelection: true
            }
        }
    }
}
