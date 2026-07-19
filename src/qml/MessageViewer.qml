import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami
import Mailo.Core

ColumnLayout {
    id: viewer
    spacing: 0

    property bool hasMessage: false
    property string viewMode: "html"

    property string fullAuthInfo: ""

    /// Reply / Reply all was clicked for the shown message.
    signal replyRequested(bool replyAll)
    /// Forward was clicked for the shown message.
    signal forwardRequested()

    function showMessage(subject, from, to, cc, date, bodyUrl, authInfo) {
        subjectLabel.text = subject.length > 0 ? subject : "(no subject)"
        fromLabel.text = from
        toLabel.text = to
        ccLabel.text = cc
        dateLabel.text = date
        fullAuthInfo = authInfo
        dkimLabel.text = condenseAuth(authInfo)
        hasMessage = true
        viewMode = "html"
        web.url = bodyUrl
    }

    // "purelymail.com; spf=pass …; dkim=fail …" → "spf=pass · dkim=fail ❗"
    function condenseAuth(authInfo) {
        if (authInfo.length === 0)
            return ""
        const verdicts = authInfo.toLowerCase().match(/\b(dkim|spf|dmarc)=[a-z]+/g)
        if (!verdicts || verdicts.length === 0)
            return ""
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

        QQC2.Label { text: "From:"; opacity: 0.6 }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                id: fromLabel
                Layout.fillWidth: true
                elide: Text.ElideRight
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
                checked: Mail.remoteContentAllowed
                visible: viewer.viewMode === "html"
                onToggled: {
                    Mail.remoteContentAllowed = checked
                    web.url = Mail.htmlViewUrl() // re-render with the new policy
                }
                QQC2.ToolTip.text: "Allow this message to load remote images, styles and fonts (JavaScript stays off)"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.Label {
                id: dateLabel
                opacity: 0.7
            }
        }

        QQC2.Label { text: "To:"; opacity: 0.6 }
        QQC2.Label {
            id: toLabel
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        QQC2.Label { text: "Cc:"; opacity: 0.6; visible: ccLabel.text.length > 0 }
        QQC2.Label {
            id: ccLabel
            Layout.fillWidth: true
            elide: Text.ElideRight
            visible: text.length > 0
        }

        QQC2.Label { text: "Subject:"; opacity: 0.6 }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: subjectLabel
                Layout.fillWidth: true
                font.bold: true
                elide: Text.ElideRight
            }
            QQC2.ToolButton {
                text: "HTML"
                checkable: true
                checked: viewer.viewMode === "html"
                onClicked: { viewer.viewMode = "html"; web.url = Mail.htmlViewUrl() }
            }
            QQC2.ToolButton {
                text: "Text"
                checkable: true
                checked: viewer.viewMode === "text"
                onClicked: { viewer.viewMode = "text"; web.url = Mail.textViewUrl() }
            }
            QQC2.ToolButton {
                text: "Source"
                checkable: true
                checked: viewer.viewMode === "source"
                onClicked: { viewer.viewMode = "source"; web.url = Mail.sourceViewUrl() }
            }
        }

        Item { visible: dkimLabel.text.length > 0 } // caption column stays empty
        QQC2.Label {
            id: dkimLabel
            visible: text.length > 0
            Layout.fillWidth: true
            elide: Text.ElideRight
            opacity: 0.8
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            QQC2.ToolTip.text: viewer.fullAuthInfo
            QQC2.ToolTip.visible: dkimHover.hovered && viewer.fullAuthInfo.length > 0
            HoverHandler { id: dkimHover }
        }
    }

    Kirigami.Separator {
        Layout.fillWidth: true
        visible: viewer.hasMessage
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
        visible: Mail.attachments.length > 0
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
                    model: Mail.attachments
                    delegate: QQC2.Button {
                        required property var modelData
                        required property int index
                        icon.name: "mail-attachment"
                        text: modelData.name + " (" + modelData.sizeText + ")"
                        onClicked: { // left click = open (risky types need confirmation)
                            if (Mail.attachmentRisky(index)) {
                                confirmOpenDialog.attachmentIndex = index
                                confirmOpenDialog.attachmentName = modelData.name
                                confirmOpenDialog.open()
                            } else {
                                Mail.openAttachment(index)
                            }
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton // right click = save to ~/Downloads
                            onTapped: Mail.saveAttachmentToDownloads(index)
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
        onAccepted: Mail.openAttachment(attachmentIndex)

        contentItem: QQC2.Label {
            text: "\"" + confirmOpenDialog.attachmentName + "\" is a script, program "
                  + "or installer. Opening it can run code on this computer.\n\n"
                  + "Only continue if you trust the sender — and remember the "
                  + "sender address itself can be forged."
            wrapMode: Text.Wrap
        }
    }

}
