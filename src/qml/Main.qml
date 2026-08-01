// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Mailo.Core

Kirigami.ApplicationWindow {
    id: root
    title: "Mailo"
    width: windowSettings.width
    height: windowSettings.height

    // Last window geometry; restored at startup, captured on close.
    // x/y are best effort: Wayland compositors place windows themselves and
    // ignore programmatic positions. -1 = never saved, let the WM place it.
    Settings {
        id: windowSettings
        category: "window"
        property int width: 1200
        property int height: 760
        property int x: -1
        property int y: -1
        property bool maximized: false
    }
    onClosing: close => {
        // VACUUM cannot be interrupted and the destructor has to join its
        // thread — quitting mid-rebuild would look like a hang, so refuse.
        if (Mail.reclaiming) {
            close.accepted = false
            return
        }
        windowSettings.maximized = root.visibility === Window.Maximized
        // Keep the last windowed geometry — maximized dimensions would make
        // un-maximizing on the next run a no-op.
        if (root.visibility === Window.Windowed) {
            windowSettings.width = root.width
            windowSettings.height = root.height
            windowSettings.x = root.x
            windowSettings.y = root.y
        }
        // Closing the mail window means quitting. Left to
        // quitOnLastWindowClosed, the process kept running in its event loop
        // with nothing on screen — ComposeSheet is a top-level Window of its
        // own that is created once and kept, so "last window" is not something
        // to rely on here.
        Qt.quit()
    }

    // Same category as the C++ trace, so QT_LOGGING_RULES='mailo.trace.debug=true'
    // (or the Settings toggle) turns both on together.
    LoggingCategory {
        id: traceLog
        name: "mailo.trace"
        defaultLogLevel: LoggingCategory.Fatal
    }

    // Persisted UI state (column order, sorting, collapsed account nodes)
    Settings {
        id: uiSettings
        category: "ui"
        property string columnOrder: "[]"
        property int sortColumn: 0
        property bool sortDescending: true
        property string collapsedAccounts: "[]"
        property int rowDensity: 1     // 0 compact, 1 medium, 2 wide
        property string bgColor: ""    // "" = theme default
        // Definable shortcuts (Look settings); QKeySequence strings.
        property string shortcutDelete: "Del"
        property string shortcutJunk: "J"
        property string shortcutCompose: "C"
        property string shortcutReply: "R"
        property string shortcutForward: "F"
        property string shortcutSelect: "Ins"
        // Compose-window shortcuts (full QKeySequence strings with modifiers).
        property string shortcutAttach: "Ctrl+Shift+A"
        property string shortcutSend: "Ctrl+Return"
        // Message-viewer shortcuts (reading pane and detached message window).
        property string shortcutFind: "Ctrl+F"
        property string shortcutSource: "Ctrl+U"
        // Color scale 1–5: shortcut + color per slot, both "" = undefined.
        // A slot with a shortcut but no color clears the mark instead.
        property string scaleKey1: ""
        property string scaleKey2: ""
        property string scaleKey3: ""
        property string scaleKey4: ""
        property string scaleKey5: ""
        property string scaleColor1: ""
        property string scaleColor2: ""
        property string scaleColor3: ""
        property string scaleColor4: ""
        property string scaleColor5: ""
    }

    // Active color quick filter (0 = off), mirrored to Mail.filterByColor().
    property int colorFilter: 0
    function scaleColorOf(i) {
        return uiSettings["scaleColor" + i]
    }

    // Mail-list row height from the density setting
    readonly property real listRowHeight:
        Kirigami.Units.gridUnit * [1.15, 1.4, 1.9][uiSettings.rowDensity]
    readonly property color panelColor: uiSettings.bgColor !== ""
        ? uiSettings.bgColor : Kirigami.Theme.backgroundColor

    // True while a text field has focus. Single-letter shortcuts must not fire
    // mid-word in the search box or a rename field — the old list-only wiring
    // got that for free, window-wide Shortcut objects have to ask.
    // Detected by properties only text editors carry: WebEngineView, buttons
    // and list views have none of them, so the reading pane still gets
    // shortcuts.
    readonly property bool textFieldFocused: {
        const item = root.activeFocusItem
        return !!item && item.hasOwnProperty("cursorPosition")
                      && item.hasOwnProperty("selectionStart")
    }
    // Also off while Settings is open: its shortcut-capture buttons read raw
    // key presses, and a window-wide Shortcut would run the very action being
    // rebound.
    /// Set when a click in Drafts starts a fetch, so the message opens in the
    /// composer once it arrives rather than in the reader.
    property bool draftEditPending: false

    readonly property bool shortcutsLive:
        !textFieldFocused && !(accountDialog && accountDialog.visible)

    // Window-wide rather than per-view: these used to be handled only by the
    // folder and message lists' Keys.onPressed, so pressing Compose while the
    // reading pane (or anything else) had focus did nothing at all.
    Shortcut {
        sequence: uiSettings.shortcutCompose
        enabled: sequence !== "" && root.shortcutsLive && Mail.hasAccount
        onActivated: composeSheet().openNew()
    }
    Shortcut {
        sequence: uiSettings.shortcutReply
        enabled: sequence !== "" && root.shortcutsLive && viewer.hasMessage
        onActivated: composeSheet().openReply(Mail.replyData(false))
    }
    Shortcut {
        sequence: uiSettings.shortcutForward
        enabled: sequence !== "" && root.shortcutsLive && viewer.hasMessage
        onActivated: composeSheet().openForward(Mail.forwardData())
    }
    Shortcut {
        sequence: uiSettings.shortcutDelete
        enabled: sequence !== "" && root.shortcutsLive
        onActivated: messageList.requestDelete()
    }
    Shortcut {
        sequence: uiSettings.shortcutJunk
        enabled: sequence !== "" && root.shortcutsLive
        onActivated: messageList.requestJunk()
    }
    Shortcut {
        sequence: uiSettings.shortcutSelect
        enabled: sequence !== "" && root.shortcutsLive
        onActivated: messageList.toggleSelectAndAdvance()
    }
    // Instantiator, not Repeater: Shortcut is not an Item and has nothing to
    // lay out.
    Instantiator {
        model: 5
        delegate: Shortcut {
            required property int index
            readonly property int slot: index + 1
            sequence: uiSettings["scaleKey" + slot]
            enabled: sequence !== "" && root.shortcutsLive
            onActivated: Mail.markMessageColor(messageList.selectedIndexes(),
                                               root.scaleColorOf(slot) !== "" ? slot : 0)
        }
    }

    Component.onCompleted: {
        // Position before maximizing, so un-maximizing lands where it was.
        if (windowSettings.x >= 0) {
            root.x = windowSettings.x
            root.y = windowSettings.y
        }
        if (windowSettings.maximized)
            root.showMaximized()
        if (Mail.hasAccount) {
            Mail.connectAccount()
            // Keyboard-ready from the start: the list has focus, and
            // autoSelect() makes the newest message current once it loads.
            messageList.forceActiveFocus()
        } else {
            accountSheet().open()
        }
    }

    Connections {
        target: Mail
        // Errors are folded into the status breadcrumb (Mail.setStatus), not
        // shown as passive popups — the status line already carries them, kept
        // short. No onErrorOccurred handler on purpose.
        // The reading pane itself renders from Mail.readingContext; this
        // handler only routes drafts into the composer.
        function onMessageLoaded(subject, from, to, cc, date, bodyUrl, authInfo) {
            if (root.draftEditPending) {
                root.draftEditPending = false
                composeSheet().openDraft(Mail.draftData())
            }
        }
        // A double-clicked message is ready: show it in its own window.
        function onMessageWindowReady(context) {
            root.openMessageWindow(context)
        }
        // Once the server refresh lands, (re)load the selected message —
        // the startup auto-select may have fired while still offline.
        function onFolderRefreshed() {
            if (messageList.currentIndex >= 0 && !viewer.hasMessage)
                fetchDebounce.restart()
            else
                messageList.autoSelect()
        }
    }

    // One window per double-clicked message; each owns its context and frees
    // it (and destroys itself) on close — no bookkeeping needed here.
    Component {
        id: messageWindowComponent
        MessageWindow {}
    }
    function openMessageWindow(context) {
        const w = messageWindowComponent.createObject(root, {
            context: context,
            ui: uiSettings
        })
        w.replyRequested.connect(replyAll =>
            composeSheet().openReply(context.replyData(replyAll)))
        w.forwardRequested.connect(() =>
            composeSheet().openForward(context.forwardData()))
        w.present()
    }

    // Both of these are built on first use, not at startup. Between them they
    // were the whole cost of the QML load — AccountSheet instantiates all five
    // settings pages (a StackLayout builds every child regardless of
    // currentIndex) and ComposeSheet a full editor window, for UI the user may
    // never open in a session.
    // A vacuum holds an exclusive lock on the whole cache for minutes, so the
    // mailbox genuinely is unavailable while it runs — every folder switch or
    // fetch would block on the lock. Rather than let the app look hung, say so
    // and take input away. No buttons: it cannot be cancelled or dismissed.
    QQC2.Dialog {
        id: reclaimDialog
        modal: true
        closePolicy: QQC2.Popup.NoAutoClose
        anchors.centerIn: parent
        parent: root.contentItem
        title: "Reclaiming disk space"
        visible: Mail.reclaiming
        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing
            QQC2.Label {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                wrapMode: Text.Wrap
                text: "Rebuilding the mail cache to return free space to the disk.\n\n"
                      + "Your mail is unavailable until this finishes, and it "
                      + "cannot be interrupted. This usually takes a few minutes."
            }
            QQC2.ProgressBar {
                Layout.fillWidth: true
                indeterminate: true
            }
        }
    }

    // Created on first use rather than at startup, and kept afterwards. Between
    // them these were essentially the whole cost of the QML load: AccountSheet
    // builds all five settings pages (a StackLayout instantiates every child
    // regardless of currentIndex) and ComposeSheet a full editor window — for
    // UI that many sessions never open.
    //
    // AccountSheet is a top-level Window of its own now (like ComposeSheet),
    // so it is parented to root only for lifetime.
    property var accountDialog: null
    Component {
        id: accountComponent
        AccountSheet { ui: uiSettings }
    }
    function accountSheet() {
        if (!accountDialog)
            accountDialog = accountComponent.createObject(root)
        return accountDialog
    }

    property var composeWindow: null
    Component {
        id: composeComponent
        ComposeSheet { ui: uiSettings }
    }
    function composeSheet() {
        if (!composeWindow)
            composeWindow = composeComponent.createObject(root)
        return composeWindow
    }

    // What is currently being dragged, and the pill that follows the cursor.
    // One shared instance — only one drag can be in flight at a time. It lives
    // in the overlay so it draws above both panes and can be positioned in
    // scene coordinates from any delegate, wherever the drag started.
    Item {
        id: dragPayload
        parent: QQC2.Overlay.overlay
        z: 9999
        visible: false
        width: dragPill.width
        height: dragPill.height

        property string kind: ""    // "messages" | "folder" | ""
        property var rows: []       // message rows, for kind "messages"
        property string mailBox: "" // dragged folder, for kind "folder"
        property string label: ""

        Drag.active: false
        Drag.source: dragPayload
        // The pill sits just off the cursor (see moveTo), so the drop point
        // is its top-left corner — i.e. the cursor itself.
        Drag.hotSpot: Qt.point(0, 0)

        /// Records what a press would drag, without starting a drag yet.
        function prepare(dragKind, dragRows, box, text) {
            kind = dragKind
            rows = dragRows
            mailBox = box
            label = text
        }
        /// Places the pill at \a scenePos (overlay coordinates).
        function moveTo(scenePos) {
            x = scenePos.x + Kirigami.Units.smallSpacing
            y = scenePos.y + Kirigami.Units.smallSpacing
        }
        function begin() {
            visible = true
            Drag.active = true
        }
        function finish() {
            if (Drag.active)
                Drag.drop()
            Drag.active = false
            visible = false
            kind = ""
            rows = []
            mailBox = ""
        }

        Rectangle {
            id: dragPill
            width: pillLabel.implicitWidth + Kirigami.Units.largeSpacing * 2
            height: pillLabel.implicitHeight + Kirigami.Units.smallSpacing * 2
            radius: Kirigami.Units.smallSpacing
            color: Kirigami.Theme.highlightColor
            border.width: 1
            border.color: Kirigami.Theme.textColor
            opacity: 0.9

            QQC2.Label {
                id: pillLabel
                anchors.centerIn: parent
                text: dragPayload.label
                color: Kirigami.Theme.highlightedTextColor
            }
        }
    }

    // Right-click menu of a folder in the sidebar.
    QQC2.Menu {
        id: folderMenu
        property string mailBox: ""
        property string name: ""

        QQC2.MenuItem {
            text: "Move to top level"
            icon.name: "go-up"
            enabled: Mail.canMoveFolder(folderMenu.mailBox, "")
            onTriggered: Mail.moveFolder(folderMenu.mailBox, "")
        }
        QQC2.MenuItem {
            text: Mail.folderDeleteIsPermanent(folderMenu.mailBox)
                  ? "Delete folder…" : "Move folder to trash…"
            icon.name: "edit-delete"
            enabled: !Mail.folderProtected(folderMenu.mailBox)
            onTriggered: {
                confirmFolderDelete.mailBox = folderMenu.mailBox
                confirmFolderDelete.name = folderMenu.name
                confirmFolderDelete.permanent =
                    Mail.folderDeleteIsPermanent(folderMenu.mailBox)
                confirmFolderDelete.open()
            }
        }
    }

    QQC2.Dialog {
        id: confirmFolderDelete
        property string mailBox: ""
        property string name: ""
        property bool permanent: false

        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: permanent ? "Delete folder permanently?" : "Move folder to trash?"

        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: confirmFolderDelete.permanent ? "Delete permanently" : "Move to trash"
                icon.name: "edit-delete"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
            }
            QQC2.Button {
                text: "Cancel"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
            }
        }
        onAccepted: Mail.deleteFolder(confirmFolderDelete.mailBox)

        // Wrapped in an Item because Text sizes itself to its content: a
        // paragraph of explanation would otherwise make the dialog as wide as
        // the sentence. The Item is what carries the width cap.
        contentItem: Item {
            implicitWidth: Kirigami.Units.gridUnit * 22
            implicitHeight: folderDeleteText.implicitHeight

            QQC2.Label {
                id: folderDeleteText
                anchors.fill: parent
                text: confirmFolderDelete.permanent
                      ? "“" + confirmFolderDelete.name + "” and everything in it "
                        + "(including any subfolders) are removed from the server "
                        + "permanently — this cannot be undone."
                      : "“" + confirmFolderDelete.name + "” and its subfolders "
                        + "are moved into the trash, with all the messages they hold. "
                        + "Deleting it again from there removes it for good."
                wrapMode: Text.Wrap
            }
        }
    }

    QQC2.Dialog {
        id: confirmPermanentDelete
        property var rows: []
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Delete permanently?"

        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: "Delete permanently"
                icon.name: "edit-delete"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
            }
            QQC2.Button {
                text: "Cancel"
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
            }
        }
        onAccepted: {
            Mail.deleteMessages(rows)
            messageList.clearSelection()
        }

        contentItem: QQC2.Label {
            text: confirmPermanentDelete.rows.length === 1
                  ? "This message is in the trash. Deleting it here removes it "
                    + "from the server permanently — this cannot be undone."
                  : confirmPermanentDelete.rows.length + " messages are in the trash. "
                    + "Deleting them here removes them from the server permanently "
                    + "— this cannot be undone."
            wrapMode: Text.Wrap
        }
    }

    pageStack.initialPage: Kirigami.Page {
        padding: 0
        background: Rectangle {
            color: root.panelColor
        }
        titleDelegate: RowLayout {
            Layout.fillWidth: true
            Kirigami.Heading {
                text: "Mailo"
                level: 2
            }
            // Version straight from QCoreApplication (main.cpp sets it from
            // MAILO_VERSION), so it can never drift from the built binary.
            QQC2.Label {
                text: "v" + Qt.application.version
                opacity: 0.7
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                Layout.alignment: Qt.AlignBaseline
            }
            // Plain arc spinner — the desktop-style BusyIndicator draws a
            // cogwheel, which reads as "settings" rather than "loading".
            Item {
                id: busySpinner
                visible: Mail.busy
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: Kirigami.Units.iconSizes.smallMedium

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
                        running: busySpinner.visible
                        from: 0
                        to: 360
                        duration: 900
                        loops: Animation.Infinite
                    }
                }
            }
            QQC2.Label {
                id: statusLabel
                Layout.fillWidth: true
                // With no account there is no activity to report, so the
                // status line would otherwise sit empty on every run until
                // one is set up. Bold because it is the only prompt on screen.
                text: Mail.hasAccount
                    ? Mail.statusText
                    // Named after the button's own tooltip rather than
                    // described by shape — the icon is theme-supplied and is
                    // not a gear.
                    : "Welcome to Mailo! Add an account to get started — Settings, top right."
                font.bold: !Mail.hasAccount
                elide: Text.ElideRight
                opacity: 0.8
                // The label elides, so the older crumbs may be off-screen —
                // right-click copies the full breadcrumb trail, and hovering
                // shows it in a tooltip.
                QQC2.ToolTip.text: Mail.statusText
                QQC2.ToolTip.visible: statusHover.hovered && Mail.statusText.length > 0
                HoverHandler { id: statusHover }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        if (Mail.statusText.length === 0)
                            return
                        Mail.copyToClipboard(Mail.statusText)
                        root.showPassiveNotification("Status copied", "short")
                    }
                }
            }
            QQC2.ToolButton {
                icon.name: "mail-message-new"
                enabled: Mail.hasAccount
                onClicked: composeSheet().openNew()
                QQC2.ToolTip.text: "Compose"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "view-refresh"
                enabled: Mail.hasAccount && !Mail.busy
                onClicked: Mail.connectAccount()
                QQC2.ToolTip.text: "Reconnect and refresh"
                QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "settings-configure"
                onClicked: accountSheet().open()
                QQC2.ToolTip.text: "Settings"
                QQC2.ToolTip.visible: hovered
            }
        }

        QQC2.SplitView {
            anchors.fill: parent
            orientation: Qt.Horizontal

            // Folder pane — one scrolling column. Every account can be open
            // at the same time; the next account continues right after the
            // previous account's folders (nothing is pinned to the bottom).
            // Only the connected account's list is live; the others show
            // their cached folder tree, and clicking a folder there switches
            // the connection over and opens it.
            ColumnLayout {
                id: folderPane
                QQC2.SplitView.preferredWidth: 220
                QQC2.SplitView.minimumWidth: 140
                spacing: 0

                property Item folderListView: null

                // Special-role folder icons, matched on the folder's own name
                // (Trash, INBOX/Spam, Junk E-mail, Deleted Items, …).
                function folderIcon(mailBox) {
                    if (mailBox.toUpperCase() === "INBOX")
                        return "mail-folder-inbox"
                    const leaf = mailBox.split(/[/.]/).pop().toLowerCase()
                    if (leaf.includes("trash") || leaf.includes("deleted"))
                        return "user-trash"
                    if (leaf.includes("spam") || leaf.includes("junk"))
                        return "mail-mark-junk"
                    if (leaf.includes("sent"))
                        return "mail-folder-sent"
                    if (leaf.includes("outbox"))
                        return "mail-folder-outbox"
                    if (leaf.includes("draft"))
                        return "document-edit"
                    return "folder-mail"
                }

                function isCollapsed(name) {
                    return JSON.parse(uiSettings.collapsedAccounts).indexOf(name) >= 0
                }
                function setCollapsed(name, collapsed) {
                    const a = JSON.parse(uiSettings.collapsedAccounts)
                    const i = a.indexOf(name)
                    if (collapsed && i < 0)
                        a.push(name)
                    else if (!collapsed && i >= 0)
                        a.splice(i, 1)
                    uiSettings.collapsedAccounts = JSON.stringify(a)
                }

                QQC2.Label {
                    visible: Mail.accountNames.length === 0
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.smallSpacing
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    text: "No account"
                    font.bold: true
                    elide: Text.ElideRight
                }

                QQC2.ScrollView {
                    id: folderScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true

                    ColumnLayout {
                        width: folderScroll.availableWidth
                        spacing: 0

                        Repeater {
                            model: Mail.accountNames

                            delegate: ColumnLayout {
                                id: accountSection
                                required property string modelData
                                required property int index

                                readonly property bool isCurrent: index === Mail.currentAccount
                                readonly property bool open: !folderPane.isCollapsed(modelData)

                                Layout.fillWidth: true
                                spacing: 0

                                // Divides this account from the previous one's
                                // folder list — above the name, not under it.
                                Kirigami.Separator {
                                    visible: accountSection.index > 0
                                    Layout.fillWidth: true
                                }
                                QQC2.ItemDelegate {
                                    Layout.fillWidth: true
                                    implicitHeight: root.listRowHeight + 2
                                    topPadding: 1
                                    bottomPadding: 1
                                    contentItem: RowLayout {
                                        spacing: Kirigami.Units.smallSpacing
                                        Kirigami.Icon {
                                            source: accountSection.open ? "arrow-down" : "arrow-right"
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                            opacity: 0.6
                                        }
                                        // Highlight-blue dot marks the active
                                        // account. The cue is carried by this
                                        // graphical accent (3:1 bar), not by
                                        // the text color — the theme highlight
                                        // blue as text is only ~2.4:1 and fails
                                        // AA, so the name keeps full-contrast
                                        // textColor (bold does the rest).
                                        Rectangle {
                                            visible: accountSection.isCurrent
                                            Layout.preferredWidth: Kirigami.Units.smallSpacing * 1.5
                                            Layout.preferredHeight: Layout.preferredWidth
                                            radius: width / 2
                                            color: Kirigami.Theme.highlightColor
                                        }
                                        QQC2.Label {
                                            Layout.fillWidth: true
                                            text: accountSection.modelData
                                            font.bold: true
                                            color: Kirigami.Theme.textColor
                                            elide: Text.ElideRight
                                        }
                                    }
                                    onClicked: folderPane.setCollapsed(accountSection.modelData,
                                                                       accountSection.open)

                                    // Dropping a folder on its account name
                                    // moves it out to the top level — the one
                                    // reparenting target that is not a row in
                                    // the tree.
                                    DropArea {
                                        id: accountDrop
                                        anchors.fill: parent

                                        readonly property bool acceptable:
                                            accountSection.isCurrent
                                            && dragPayload.kind === "folder"
                                            && Mail.canMoveFolder(dragPayload.mailBox, "")

                                        onEntered: drag => drag.accepted = acceptable
                                        onDropped: drop => {
                                            if (!acceptable) {
                                                drop.accepted = false
                                                return
                                            }
                                            Mail.moveFolder(dragPayload.mailBox, "")
                                        }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        visible: accountDrop.containsDrag && accountDrop.acceptable
                                        color: Qt.alpha(Kirigami.Theme.highlightColor, 0.25)
                                        border.width: 2
                                        border.color: Kirigami.Theme.highlightColor
                                        radius: Kirigami.Units.smallSpacing
                                    }
                                }

                                // Live folder list of the connected account
                                ListView {
                                    id: folderList
                                    visible: accountSection.open && accountSection.isCurrent
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: visible ? contentHeight : 0
                                    interactive: false
                                    model: accountSection.isCurrent ? Mail.folderModel : null
                                    keyNavigationEnabled: true
                                    activeFocusOnTab: accountSection.isCurrent
                                    Keys.onPressed: event => {
                                        // Arrow keys are the one case where
                                        // moving the selection should open the
                                        // folder. Arming the debounce from the
                                        // key press (rather than from
                                        // currentIndex changing) means only a
                                        // real keystroke can ever open one.
                                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down
                                                || event.key === Qt.Key_PageUp
                                                || event.key === Qt.Key_PageDown
                                                || event.key === Qt.Key_Home
                                                || event.key === Qt.Key_End)
                                            folderOpenDebounce.restart()
                                        // Mail shortcuts are window-wide
                                        // Shortcut objects now; handling them
                                        // here too would fire them twice.
                                    }

                                    property bool live: accountSection.isCurrent
                                    onLiveChanged: {
                                        if (live)
                                            folderPane.folderListView = folderList
                                    }
                                    Component.onCompleted: {
                                        if (live)
                                            folderPane.folderListView = folderList
                                    }

                                    // True while currentIndex is being moved to
                                    // match the folder that is already open, so
                                    // the debounce below does not treat that as
                                    // the user asking to open something.
                                    property bool syncingIndex: false

                                    // The model is rebuilt on every account
                                    // switch and folder refresh, which snaps
                                    // currentIndex back to row 0. Left alone
                                    // that fired openCurrent() for INBOX and
                                    // overrode the folder the user had just
                                    // clicked — including the one an account
                                    // switch was still in the middle of opening.
                                    function syncToOpenFolder() {
                                        if (!live)
                                            return
                                        const open = Mail.selectedFolder
                                        // count === 0 means the model is mid-
                                        // rebuild; setting an index now would
                                        // just be undone by the repopulation.
                                        if (!open || count === 0)
                                            return
                                        // Ask the model, not the view: a folder
                                        // scrolled out of sight has no delegate,
                                        // so itemAtIndex() returns null for it
                                        // and the row would never be found.
                                        const row = Mail.folderModel.rowForMailBox(open)
                                        if (row < 0 || row === currentIndex)
                                            return
                                        syncingIndex = true
                                        currentIndex = row
                                        syncingIndex = false
                                    }
                                    Connections {
                                        target: Mail
                                        function onSelectedFolderChanged() {
                                            folderList.syncToOpenFolder()
                                        }
                                    }
                                    onCountChanged: {
                                        console.debug(traceLog, "[qml] count -> " + count
                                                      + " idx=" + currentIndex
                                                      + " open=" + Mail.selectedFolder)
                                        // A repopulated model is never the user
                                        // asking for a folder — cancel any
                                        // auto-open the reset index just armed.
                                        folderOpenDebounce.stop()
                                        rebuildSettle.restart()
                                        syncToOpenFolder()
                                    }

                                    function openCurrent() {
                                        // Model-backed, for the same reason as
                                        // syncToOpenFolder above.
                                        const mailBox = Mail.folderModel.mailBoxAt(currentIndex)
                                        console.debug(traceLog, "[qml] openCurrent idx="
                                                      + currentIndex + " mailBox=" + mailBox
                                                      + " open=" + Mail.selectedFolder)
                                        if (mailBox && mailBox !== Mail.selectedFolder
                                                && Mail.folderModel.selectableAt(currentIndex)) {
                                            messageList.currentIndex = -1
                                            messageList.openedUid = -1
                                            messageList.clearSelection()
                                            Mail.openFolder(mailBox)
                                        }
                                    }
                                    Keys.onReturnPressed: openCurrent()
                                    Keys.onEnterPressed: openCurrent()
                                    Keys.onRightPressed: messageList.forceActiveFocus()

                                    // Key navigation opens the highlighted
                                    // folder by itself — debounced so holding
                                    // an arrow key doesn't open every folder
                                    // it passes over.
                                    Timer {
                                        id: folderOpenDebounce
                                        interval: 300
                                        onTriggered: folderList.openCurrent()
                                    }
                                    // Deliberately NOT hooked to open a folder.
                                    // currentIndex moves for reasons that are
                                    // not the user: a model reset snaps it to
                                    // 0 or -1, and ListView also re-adjusts it
                                    // asynchronously while laying out, i.e.
                                    // after any "I am syncing" flag has been
                                    // cleared. Auto-opening from here made the
                                    // selection walk the list opening folders
                                    // as it went. Only real input opens now:
                                    // a click on the delegate, Return/Enter, or
                                    // arrow-key navigation (armed in
                                    // Keys.onPressed below).
                                    onCurrentIndexChanged: {
                                        console.debug(traceLog, "[qml] currentIndex -> "
                                                      + currentIndex + " syncing=" + syncingIndex
                                                      + " open=" + Mail.selectedFolder
                                                      + " count=" + count)
                                        // While a rebuild settles, ListView
                                        // snaps the cursor to row 0 (INBOX) and
                                        // to -1 before landing. Nothing opens
                                        // from here any more, so putting it
                                        // straight back is safe — and stops
                                        // INBOX drawing as the current item for
                                        // a frame. Outside that window the
                                        // cursor is the user's to move.
                                        if (!syncingIndex && rebuildSettle.running)
                                            syncToOpenFolder()
                                    }

                                    // Runs for a moment after any model change;
                                    // see onCurrentIndexChanged above.
                                    Timer {
                                        id: rebuildSettle
                                        interval: 600
                                    }

                                    delegate: QQC2.ItemDelegate {
                                        id: folderDelegate
                                        required property string name
                                        required property string mailBox
                                        required property int level
                                        required property bool selectable
                                        required property bool hasChildren
                                        required property bool expanded
                                        required property int index

                                        width: folderList.width
                                        implicitHeight: root.listRowHeight + 2
                                        topPadding: 1
                                        bottomPadding: 1
                                        text: name
                                        enabled: selectable || hasChildren
                                        leftPadding: Kirigami.Units.largeSpacing + Kirigami.Units.gridUnit
                                                     + level * Kirigami.Units.gridUnit
                                        icon.name: folderPane.folderIcon(mailBox)
                                        icon.width: Kirigami.Units.iconSizes.small
                                        icon.height: Kirigami.Units.iconSizes.small
                                        icon.color: Qt.alpha(Kirigami.Theme.textColor, 0.55)
                                        // Follows the folder that is actually
                                        // open, not the view's cursor. During a
                                        // model rebuild currentIndex churns
                                        // through 0 (INBOX) and -1 before
                                        // settling, which made INBOX flash as
                                        // selected on every account switch.
                                        highlighted: folderDelegate.mailBox === Mail.selectedFolder

                                        // Opening the folder — reached from the
                                        // click handler below rather than from
                                        // ItemDelegate.onClicked, because the
                                        // drag source takes the press.
                                        function activate() {
                                            if (!selectable) { // container-only folder: toggle instead
                                                Mail.folderModel.toggleExpanded(index)
                                                return
                                            }
                                            // Clicking a folder works its
                                            // subtree like the arrow does:
                                            // reveal it, and fold it away again
                                            // on a second click. "Second" means
                                            // this folder is already the open
                                            // one — clicking a different
                                            // expanded parent to read its mail
                                            // should not collapse it. Children
                                            // sort below this row, so index
                                            // stays valid either way.
                                            if (hasChildren && expanded
                                                    && Mail.selectedFolder === mailBox) {
                                                Mail.folderModel.toggleExpanded(index)
                                                return // already open; nothing to re-fetch
                                            }
                                            Mail.folderModel.expandRow(index)
                                            folderList.currentIndex = index
                                            folderList.forceActiveFocus()
                                            // A click opens right away — drop
                                            // the key-navigation debounce the
                                            // currentIndex change just armed.
                                            folderOpenDebounce.stop()
                                            messageList.currentIndex = -1
                                            messageList.openedUid = -1
                                            messageList.clearSelection()
                                            Mail.openFolder(mailBox)
                                        }

                                        // Drop target: messages land in this
                                        // folder, another folder is reparented
                                        // under it. What a drop would mean is
                                        // asked of the backend, so a row only
                                        // lights up when the drop would work.
                                        DropArea {
                                            id: folderDrop
                                            anchors.fill: parent

                                            readonly property bool acceptable:
                                                dragPayload.kind === "messages"
                                                ? (folderDelegate.selectable
                                                   && folderDelegate.mailBox !== Mail.selectedFolder)
                                                : dragPayload.kind === "folder"
                                                  && Mail.canMoveFolder(dragPayload.mailBox,
                                                                        folderDelegate.mailBox)

                                            onEntered: drag => drag.accepted = acceptable
                                            onDropped: drop => {
                                                if (!acceptable) {
                                                    drop.accepted = false
                                                    return
                                                }
                                                if (dragPayload.kind === "messages") {
                                                    Mail.moveMessagesTo(dragPayload.rows,
                                                                        folderDelegate.mailBox)
                                                    messageList.clearSelection()
                                                } else {
                                                    Mail.moveFolder(dragPayload.mailBox,
                                                                    folderDelegate.mailBox)
                                                }
                                            }
                                        }

                                        // Outline marking the row a drop would
                                        // land in. Deliberately an outline plus
                                        // a light fill rather than a color
                                        // change: it has to read on top of the
                                        // selection highlight as well.
                                        Rectangle {
                                            anchors.fill: parent
                                            visible: folderDrop.containsDrag && folderDrop.acceptable
                                            color: Qt.alpha(Kirigami.Theme.highlightColor, 0.25)
                                            border.width: 2
                                            border.color: Kirigami.Theme.highlightColor
                                            radius: Kirigami.Units.smallSpacing
                                        }

                                        // Clicks and the folder drag both come
                                        // from here: ItemDelegate.onClicked
                                        // never fires once this takes the press.
                                        MouseArea {
                                            id: folderMouse
                                            anchors.fill: parent
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            // Dragging a folder moves a whole
                                            // mailbox — an easy accident with
                                            // the default few pixels.
                                            drag.threshold: Kirigami.Units.gridUnit
                                            // INBOX and the special-use folders
                                            // cannot be moved at all, so they
                                            // never start a drag.
                                            drag.target: Mail.folderProtected(folderDelegate.mailBox)
                                                         ? null : dragPayload

                                            onPressed: mouse => {
                                                if (mouse.button !== Qt.LeftButton)
                                                    return
                                                dragPayload.prepare("folder", [],
                                                                    folderDelegate.mailBox,
                                                                    folderDelegate.name)
                                                dragPayload.moveTo(folderDelegate.mapToItem(
                                                    QQC2.Overlay.overlay, mouse.x, mouse.y))
                                            }
                                            drag.onActiveChanged: {
                                                if (folderMouse.drag.active)
                                                    dragPayload.begin()
                                                else
                                                    dragPayload.finish()
                                            }
                                            onClicked: mouse => {
                                                if (mouse.button === Qt.RightButton) {
                                                    folderMenu.mailBox = folderDelegate.mailBox
                                                    folderMenu.name = folderDelegate.name
                                                    folderMenu.popup()
                                                    return
                                                }
                                                folderDelegate.activate()
                                            }
                                        }

                                        Kirigami.Icon {
                                            visible: folderDelegate.hasChildren
                                            x: Kirigami.Units.smallSpacing
                                               + folderDelegate.level * Kirigami.Units.gridUnit
                                            anchors.verticalCenter: parent.verticalCenter
                                            source: folderDelegate.expanded ? "arrow-down" : "arrow-right"
                                            width: Kirigami.Units.iconSizes.small
                                            height: width
                                            opacity: 0.6

                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -Kirigami.Units.smallSpacing
                                                onClicked: Mail.folderModel.toggleExpanded(folderDelegate.index)
                                            }
                                        }
                                    }
                                }

                                // Cached folder tree of an account that is
                                // open in the panel but not connected.
                                ColumnLayout {
                                    visible: accountSection.open && !accountSection.isCurrent
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Repeater {
                                        // Depends on currentAccount (connection
                                        // moved) and cachedFolderRevision
                                        // (collapse toggled) so the list refreshes.
                                        model: accountSection.isCurrent
                                               ? []
                                               : (Mail.currentAccount,
                                                  Mail.cachedFolderRevision,
                                                  Mail.cachedFolderList(accountSection.index))

                                        delegate: QQC2.ItemDelegate {
                                            id: cachedFolderDelegate
                                            required property var modelData

                                            Layout.fillWidth: true
                                            implicitHeight: root.listRowHeight + 2
                                            topPadding: 1
                                            bottomPadding: 1
                                            text: modelData.name
                                            leftPadding: Kirigami.Units.largeSpacing + Kirigami.Units.gridUnit
                                                         + modelData.level * Kirigami.Units.gridUnit
                                            icon.name: folderPane.folderIcon(modelData.mailBox)
                                            icon.width: Kirigami.Units.iconSizes.small
                                            icon.height: Kirigami.Units.iconSizes.small
                                            icon.color: Qt.alpha(Kirigami.Theme.textColor, 0.55)
                                            onClicked: {
                                                messageList.currentIndex = -1
                                                messageList.openedUid = -1
                                                messageList.clearSelection()
                                                Mail.openFolderInAccount(accountSection.index,
                                                                         modelData.mailBox)
                                            }

                                            // Same expand/collapse arrow as the
                                            // connected account's tree.
                                            Kirigami.Icon {
                                                visible: cachedFolderDelegate.modelData.hasChildren
                                                x: Kirigami.Units.smallSpacing
                                                   + cachedFolderDelegate.modelData.level * Kirigami.Units.gridUnit
                                                anchors.verticalCenter: parent.verticalCenter
                                                source: cachedFolderDelegate.modelData.expanded
                                                        ? "arrow-down" : "arrow-right"
                                                width: Kirigami.Units.iconSizes.small
                                                height: width
                                                opacity: 0.6

                                                MouseArea {
                                                    anchors.fill: parent
                                                    anchors.margins: -Kirigami.Units.smallSpacing
                                                    onClicked: Mail.toggleCachedCollapsed(
                                                                   accountSection.index,
                                                                   cachedFolderDelegate.modelData.mailBox)
                                                }
                                            }
                                        }
                                    }

                                    QQC2.Label {
                                        visible: parent.visible
                                                 && Mail.cachedFolderList(accountSection.index).length === 0
                                        Layout.fillWidth: true
                                        leftPadding: Kirigami.Units.largeSpacing + Kirigami.Units.gridUnit
                                        text: "Not synced yet"
                                        opacity: 0.8
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Right side: message list on top, viewer below
            QQC2.SplitView {
                id: rightSplit
                QQC2.SplitView.fillWidth: true
                QQC2.SplitView.minimumWidth: 300
                orientation: Qt.Vertical

                // Wider, hover-highlighted grab area for the list/viewer divider
                handle: Rectangle {
                    implicitHeight: 6
                    color: QQC2.SplitHandle.hovered || QQC2.SplitHandle.pressed
                           ? Kirigami.Theme.highlightColor : "transparent"
                    opacity: QQC2.SplitHandle.pressed ? 0.6 : 0.3
                    Kirigami.Separator {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                    }
                }

            // Message list pane
            ColumnLayout {
                id: messagePane
                QQC2.SplitView.preferredHeight: rightSplit.height / 2
                QQC2.SplitView.minimumHeight: 160
                spacing: 0

                // Column layout shared by the header row and every message row.
                // Order is user-adjustable by dragging headers; weight 0 marks
                // the fixed-width attachment icon column.
                property ListModel columns: ListModel {
                    ListElement { colId: "attach"; title: ""; sortCol: 3; weight: 0 }
                    ListElement { colId: "subject"; title: "Subject"; sortCol: 2; weight: 4 }
                    ListElement { colId: "from"; title: "From"; sortCol: 1; weight: 3 }
                    ListElement { colId: "date"; title: "Date"; sortCol: 0; weight: 2 }
                }
                property real fixedColumnWidth: Kirigami.Units.gridUnit * 2

                function saveColumnOrder() {
                    const ids = []
                    for (let i = 0; i < columns.count; i++)
                        ids.push(columns.get(i).colId)
                    uiSettings.columnOrder = JSON.stringify(ids)
                }
                Component.onCompleted: {
                    // Restore the saved column order (ignore unknown/missing ids)
                    const saved = JSON.parse(uiSettings.columnOrder)
                    let target = 0
                    for (const colId of saved) {
                        for (let i = target; i < columns.count; i++) {
                            if (columns.get(i).colId === colId) {
                                if (i !== target)
                                    columns.move(i, target, 1)
                                target++
                                break
                            }
                        }
                    }
                }
                function columnWidth(weight, totalWidth) {
                    if (weight === 0)
                        return fixedColumnWidth
                    let flexTotal = 0
                    let fixedTotal = 0
                    for (let i = 0; i < columns.count; i++) {
                        const w = columns.get(i).weight
                        if (w === 0)
                            fixedTotal += fixedColumnWidth
                        else
                            flexTotal += w
                    }
                    return Math.max(0, (totalWidth - fixedTotal) * weight / flexTotal)
                }

                // Search row
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.SearchField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: "Search… (/pattern/ = regex on loaded)"
                        // Kirigami claims Ctrl+F (StandardKey.Find) here by
                        // default, which collided with the viewer's find bar —
                        // two shortcuts on one sequence and Qt activates
                        // neither. Ctrl+F searches inside the open message;
                        // Ctrl+Shift+F searches the mailbox.
                        focusSequences: ["Ctrl+Shift+F"]
                        onAccepted: Mail.searchMessages(text, searchFieldBox.currentIndex)
                        onTextChanged: {
                            if (text.length === 0)
                                Mail.clearSearch()
                        }
                        Keys.onEscapePressed: {
                            text = ""
                            messageList.forceActiveFocus()
                        }
                    }
                    QQC2.ComboBox {
                        id: searchFieldBox
                        model: ["Everything", "Subject", "From", "Body"]
                        implicitWidth: Kirigami.Units.gridUnit * 7
                    }

                    // Quick filter by color mark — one square per defined
                    // scale color; click filters, click again clears.
                    Repeater {
                        model: [1, 2, 3, 4, 5]
                        Rectangle {
                            required property int modelData
                            readonly property string scaleColor:
                                root.scaleColorOf(modelData)
                            visible: scaleColor !== ""
                            width: Kirigami.Units.gridUnit * 1.1
                            height: width
                            radius: 3
                            color: scaleColor !== "" ? scaleColor : "transparent"
                            border.width: root.colorFilter === modelData ? 2 : 1
                            border.color: root.colorFilter === modelData
                                          ? Kirigami.Theme.highlightColor
                                          : Qt.alpha(Kirigami.Theme.textColor, 0.55)
                            // Deliberately a MouseArea, not a Button: filtering
                            // must never steal keyboard focus from the search
                            // field or the message list.
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.colorFilter = root.colorFilter === parent.modelData
                                        ? 0 : parent.modelData
                                    Mail.filterByColor(root.colorFilter)
                                }
                            }
                            QQC2.ToolTip.text: root.colorFilter === modelData
                                ? "Clear color filter"
                                : "Show only messages marked with this color"
                            QQC2.ToolTip.visible: squareHover.hovered
                            HoverHandler { id: squareHover }
                        }
                    }
                }

                // Column headers: click to sort, drag onto another header to
                // reorder the columns.
                Item {
                    id: columnHeader
                    Layout.fillWidth: true
                    implicitHeight: Kirigami.Units.gridUnit * 1.6

                    // model order: 0 date, 1 from, 2 subject, 3 attachment
                    property int sortColumn: uiSettings.sortColumn
                    property bool sortDescending: uiSettings.sortDescending

                    function toggle(col) {
                        if (sortColumn === col)
                            sortDescending = !sortDescending
                        else {
                            sortColumn = col
                            // dates newest-first, attachments-first by default
                            sortDescending = (col === 0 || col === 3)
                        }
                        uiSettings.sortColumn = sortColumn
                        uiSettings.sortDescending = sortDescending
                        Mail.messageModel.sortBy(sortColumn, sortDescending)
                    }
                    Component.onCompleted: {
                        if (sortColumn !== 0 || !sortDescending)
                            Mail.messageModel.sortBy(sortColumn, sortDescending)
                    }

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: messagePane.columns
                            delegate: Item {
                                id: headerCell
                                required property string colId
                                required property string title
                                required property int sortCol
                                required property real weight
                                required property int index

                                width: messagePane.columnWidth(weight, columnHeader.width)
                                height: columnHeader.height
                                z: headerMouse.drag.active ? 10 : 0

                                DropArea {
                                    anchors.fill: parent
                                    onDropped: drop => {
                                        const from = drop.source.headerIndex
                                        if (from !== headerCell.index) {
                                            messagePane.columns.move(from, headerCell.index, 1)
                                            messagePane.saveColumnOrder()
                                        }
                                    }
                                }

                                Rectangle {
                                    id: headerContent
                                    property int headerIndex: headerCell.index
                                    width: headerCell.width
                                    height: headerCell.height
                                    color: headerMouse.drag.active
                                           ? Kirigami.Theme.alternateBackgroundColor
                                           : "transparent"
                                    opacity: headerMouse.drag.active ? 0.8 : 1

                                    Drag.active: headerMouse.drag.active
                                    Drag.source: headerContent
                                    Drag.hotSpot: Qt.point(width / 2, height / 2)

                                    Kirigami.Icon {
                                        visible: headerCell.colId === "attach"
                                        anchors.centerIn: parent
                                        source: "mail-attachment"
                                        width: Kirigami.Units.iconSizes.small
                                        height: width
                                        opacity: 0.7
                                    }
                                    QQC2.Label {
                                        visible: headerCell.colId !== "attach"
                                        anchors.left: parent.left
                                        anchors.right: sortArrow.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: Kirigami.Units.smallSpacing
                                        text: headerCell.title
                                        elide: Text.ElideRight
                                        font.bold: columnHeader.sortColumn === headerCell.sortCol
                                    }
                                    // Discreet sort indicator on the right edge
                                    Kirigami.Icon {
                                        id: sortArrow
                                        anchors.right: parent.right
                                        anchors.rightMargin: Kirigami.Units.smallSpacing
                                        anchors.verticalCenter: parent.verticalCenter
                                        source: columnHeader.sortDescending ? "arrow-down" : "arrow-up"
                                        visible: headerCell.sortCol >= 0
                                                 && columnHeader.sortColumn === headerCell.sortCol
                                        width: Kirigami.Units.iconSizes.small * 0.75
                                        height: width
                                        opacity: 0.55
                                    }

                                    MouseArea {
                                        id: headerMouse
                                        anchors.fill: parent
                                        drag.target: headerContent
                                        drag.axis: Drag.XAxis
                                        onClicked: {
                                            if (headerCell.sortCol >= 0)
                                                columnHeader.toggle(headerCell.sortCol)
                                        }
                                        onReleased: {
                                            headerContent.Drag.drop()
                                            headerContent.x = 0
                                            headerContent.y = 0
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                }

                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ListView {
                        id: messageList
                        model: Mail.messageModel
                        currentIndex: -1
                        keyNavigationEnabled: true
                        activeFocusOnTab: true

                        // The message the cursor is on, tracked by uid so it
                        // survives a model reset renumbering the rows. Uids are
                        // only unique within a folder, so every folder change
                        // clears this alongside currentIndex. "real", not
                        // "int": IMAP uids run past 2^31.
                        property real openedUid: -1

                        // Multi-selection (ctrl+click toggles, shift+click ranges)
                        property var selectedSet: ({})
                        property int selectionRev: 0
                        property int selectionAnchor: -1

                        function isSelected(i) {
                            void selectionRev
                            return selectedSet[i] === true
                        }
                        function clearSelection() {
                            selectedSet = {}
                            selectionAnchor = -1
                            selectionRev++
                        }
                        // A plain cursor move: no explicit multi-selection —
                        // the row is implicitly selected by being current
                        // (highlight and selectedIndexes() both cover that).
                        // Keeping the set empty lets the select shortcut
                        // toggle the current row ON with its first press.
                        function selectSingle(i) {
                            selectedSet = {}
                            selectionAnchor = i
                            selectionRev++
                        }
                        function toggleSelect(i) {
                            if (selectedSet[i])
                                delete selectedSet[i]
                            else
                                selectedSet[i] = true
                            selectionAnchor = i
                            selectionRev++
                        }
                        // Toggle-select the current row and step down one —
                        // repeated presses select a run (file-manager style).
                        // The advance must not collapse the selection like a
                        // normal cursor move does.
                        property bool preserveSelection: false
                        function toggleSelectAndAdvance() {
                            if (currentIndex < 0)
                                return
                            toggleSelect(currentIndex)
                            if (currentIndex < count - 1) {
                                preserveSelection = true
                                currentIndex++
                                preserveSelection = false
                            }
                        }
                        function selectRange(a, b) {
                            const s = {}
                            for (let i = Math.min(a, b); i <= Math.max(a, b); i++)
                                s[i] = true
                            selectedSet = s
                            selectionRev++
                        }
                        function selectedIndexes() {
                            const out = []
                            for (const k in selectedSet) {
                                if (selectedSet[k])
                                    out.push(parseInt(k))
                            }
                            if (out.length === 0 && currentIndex >= 0)
                                out.push(currentIndex)
                            return out
                        }
                        function requestDelete() {
                            const rows = selectedIndexes()
                            console.info("mailo: requestDelete rows", JSON.stringify(rows),
                                         "isTrash", Mail.isTrashFolder())
                            if (rows.length === 0)
                                return
                            if (Mail.isTrashFolder()) {
                                confirmPermanentDelete.rows = rows
                                confirmPermanentDelete.open()
                            } else {
                                Mail.deleteMessages(rows)
                                clearSelection()
                            }
                        }
                        function requestJunk() {
                            const rows = selectedIndexes()
                            if (rows.length === 0)
                                return
                            Mail.markAsJunk(rows)
                            clearSelection()
                        }

                        // Row indexes shift on re-sort/search — selections
                        // would silently point at the wrong messages.
                        Connections {
                            target: Mail.messageModel
                            function onModelReset() {
                                messageList.clearSelection()
                                // The rows under the cursor are different ones
                                // now (search/filter/sort) — currentIndex often
                                // keeps its old number, so no change signal
                                // fires and the preview would show the previous
                                // message.
                                if (messageList.count <= 0) {
                                    // openedUid deliberately survives: opening a
                                    // folder clears the model and *then* fills
                                    // it, so every refresh passes through an
                                    // empty model. Forgetting the user's pick
                                    // here made the restore below a no-op — the
                                    // first reset wiped it, the second snapped
                                    // to row 0. A real folder change clears it
                                    // explicitly at the click instead.
                                    messageList.currentIndex = -1
                                    console.info("mailo: msg reset: empty, keeping uid",
                                                messageList.openedUid)
                                    return
                                }
                                // A message the user opened is followed by uid,
                                // not by row number. An account switch resets
                                // this model again when the folder refresh
                                // lands, and snapping to row 0 unconditionally
                                // threw away a message clicked in between —
                                // the click registered, then the reset moved
                                // the cursor back to the top.
                                const row = messageList.openedUid >= 0
                                    ? Mail.messageModel.rowForUid(messageList.openedUid) : -1
                                messageList.currentIndex = row >= 0 ? row : 0
                                // Set explicitly: assigning the same number
                                // fires no change signal, which would leave a
                                // uid here that is no longer under the cursor.
                                // Only when the restore actually failed — if
                                // the message was found, the cursor is already
                                // on it and its uid is the one to keep.
                                if (row < 0) {
                                    messageList.openedUid =
                                        Mail.messageModel.uidAt(messageList.currentIndex)
                                }
                                console.info("mailo: msg reset: count", messageList.count,
                                            "wanted uid", messageList.openedUid,
                                            "-> row", row, "current", messageList.currentIndex)
                                // Only when the cursor landed on a *different*
                                // message than the viewer is showing. A
                                // successful restore means the same mail is
                                // still under the cursor, and re-fetching it
                                // re-parsed the MIME and reloaded the web view
                                // on every refresh — three times per message
                                // during a reconnect, all on the GUI thread.
                                if (row < 0)
                                    fetchDebounce.restart()
                            }
                            // Incremental inserts (appendHeaders: search local
                            // merge, load-more) shift every row at/after the
                            // insertion point. selectedSet, selectionAnchor and
                            // currentIndex are stored as row numbers, so without
                            // remapping the highlight sticks to whatever message
                            // now sits at the old index — leaving the real row
                            // unhighlighted and a stale highlight behind.
                            function onRowsInserted(parent, first, last) {
                                const shift = last - first + 1
                                const remap = k => (k >= first ? k + shift : k)
                                const next = {}
                                for (const key in messageList.selectedSet) {
                                    if (messageList.selectedSet[key])
                                        next[remap(parseInt(key))] = true
                                }
                                messageList.selectedSet = next
                                if (messageList.selectionAnchor >= 0)
                                    messageList.selectionAnchor = remap(messageList.selectionAnchor)
                                if (messageList.currentIndex >= first)
                                    messageList.currentIndex = remap(messageList.currentIndex)
                                messageList.selectionRev++
                            }
                            function onRowsRemoved(parent, first, last) {
                                const shift = last - first + 1
                                const remap = k => (k > last ? k - shift : k)
                                const next = {}
                                for (const key in messageList.selectedSet) {
                                    const k = parseInt(key)
                                    if (messageList.selectedSet[key] && (k < first || k > last))
                                        next[remap(k)] = true
                                }
                                messageList.selectedSet = next
                                const a = messageList.selectionAnchor
                                messageList.selectionAnchor =
                                    (a >= first && a <= last) ? -1
                                    : (a > last ? a - shift : a)
                                messageList.selectionRev++

                                // Keep the preview in sync with what now sits
                                // under the cursor. If the current row itself
                                // was removed, currentIndex often keeps its old
                                // number and points at a *different* message,
                                // yet no onCurrentIndexChanged fires — so the
                                // preview would keep showing the deleted mail.
                                // Re-anchor and re-fetch explicitly.
                                const cur = messageList.currentIndex
                                if (cur >= first && cur <= last) {
                                    // The current row was deleted: land on the
                                    // row that took its place (clamped to end).
                                    const target = Math.min(first, messageList.count - 1)
                                    if (target < 0) {
                                        messageList.currentIndex = -1
                                        viewer.clear()
                                    } else if (messageList.currentIndex === target) {
                                        // Same number, new message → force a fetch.
                                        Mail.fetchMessage(target)
                                    } else {
                                        messageList.currentIndex = target
                                    }
                                } else if (cur > last) {
                                    // Rows above the cursor went away; its number
                                    // shifts but the message is the same — just
                                    // keep the highlight in the right place.
                                    messageList.currentIndex = cur - shift
                                }
                            }
                        }

                        // Fetch older messages when scrolled (or key-navigated) to the end.
                        onAtYEndChanged: {
                            if (atYEnd && count > 0)
                                Mail.loadMoreMessages()
                        }

                        // Debounce so holding an arrow key doesn't fetch every row.
                        Timer {
                            id: fetchDebounce
                            interval: 150
                            onTriggered: {
                                if (messageList.currentIndex >= 0)
                                    Mail.fetchMessage(messageList.currentIndex)
                            }
                        }
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0) {
                                // Moving the cursor (keyboard or click) always
                                // collapses any multi-selection to that row —
                                // otherwise the clicked row stays highlighted
                                // while the arrow keys move a second one.
                                // Exception: the select-and-advance shortcut
                                // moves the cursor without dropping the set.
                                if (!preserveSelection)
                                    selectSingle(currentIndex)
                                // Record the pick now, not when the debounced
                                // fetch fires: a reset landing inside those
                                // 150 ms is exactly the case this exists for.
                                // Only when the row names a real message —
                                // emptying the model drives the cursor to 0
                                // with nothing behind it, and recording that
                                // -1 threw away the uid this is meant to keep.
                                const uid = Mail.messageModel.uidAt(currentIndex)
                                if (uid >= 0)
                                    openedUid = uid
                                console.info("mailo: msg cursor ->", currentIndex,
                                            "uid", uid, "kept", openedUid)
                                fetchDebounce.restart()
                            }
                        }
                        // Auto-select (and show) the newest message when a
                        // folder's list appears. The cached list can already
                        // be populated before this view exists (constructor
                        // preload), so check at creation too — onCountChanged
                        // alone never fires in that case.
                        function autoSelect() {
                            if (count > 0 && currentIndex < 0)
                                currentIndex = 0
                        }
                        onCountChanged: autoSelect()
                        Component.onCompleted: autoSelect()
                        Keys.onLeftPressed: {
                            if (folderPane.folderListView)
                                folderPane.folderListView.forceActiveFocus()
                        }

                        // Hover prefetch: dwell on a row for a beat and its
                        // body is quietly cached before you even click.
                        Timer {
                            id: hoverPrefetch
                            interval: 300
                            property int row: -1
                            onTriggered: {
                                if (row >= 0)
                                    Mail.prefetchMessage(row)
                            }
                        }

                        delegate: QQC2.ItemDelegate {
                            id: msgDelegate
                            required property string subject
                            required property string from
                            required property string date
                            required property bool seen
                            required property bool suspicious
                            required property bool hasAttachment
                            required property bool calendarAttachment
                            required property string authInfo
                            required property int colorLabel
                            required property int index

                            // The message's color-scale mark (empty if none).
                            // Shown as a row background tint rather than as the
                            // text color, so text keeps full theme contrast and
                            // an arbitrary user color never becomes unreadable.
                            readonly property string markColor:
                                colorLabel > 0 ? root.scaleColorOf(colorLabel) : ""

                            width: messageList.width
                            // Row height comes from the density setting alone;
                            // fixed slim padding keeps the density steps even.
                            topPadding: 1
                            bottomPadding: 1
                            highlighted: messageList.currentIndex === index
                                         || messageList.isSelected(index)

                            // Row background: selection highlight, then hover,
                            // then the color-scale mark as a light tint. The
                            // mark shows as a tint only when the row is NOT
                            // selected ("moved away"); a selected marked row
                            // shows the mark on its TEXT instead (see below).
                            background: Rectangle {
                                color: msgDelegate.highlighted
                                        ? Kirigami.Theme.highlightColor
                                        : msgDelegate.hovered
                                          ? Qt.alpha(Kirigami.Theme.highlightColor, 0.2)
                                          : msgDelegate.markColor !== ""
                                            ? Qt.alpha(msgDelegate.markColor, 0.22)
                                            : "transparent"
                            }

                            onHoveredChanged: {
                                if (hovered) {
                                    hoverPrefetch.row = index
                                    hoverPrefetch.restart()
                                }
                            }

                            MouseArea {
                                id: msgMouse
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                // Past this the press becomes a drag and the
                                // list stops scrolling under it; short enough
                                // to feel immediate, long enough that a click
                                // with a shaky hand is still a click.
                                drag.threshold: Kirigami.Units.gridUnit
                                drag.target: dragPayload

                                onPressed: mouse => {
                                    // A drag carries the selection when the
                                    // pressed row is part of it, and just that
                                    // row otherwise — the selection itself is
                                    // only changed on release (onClicked), so
                                    // dragging never silently reselects.
                                    const rows = messageList.isSelected(msgDelegate.index)
                                               ? messageList.selectedIndexes()
                                               : [msgDelegate.index]
                                    dragPayload.prepare(
                                        "messages", rows, "",
                                        rows.length === 1 ? msgDelegate.subject
                                                          : rows.length + " messages")
                                    dragPayload.moveTo(msgDelegate.mapToItem(
                                        QQC2.Overlay.overlay, mouse.x, mouse.y))
                                }
                                drag.onActiveChanged: {
                                    if (msgMouse.drag.active)
                                        dragPayload.begin()
                                    else
                                        dragPayload.finish()
                                }
                                onClicked: mouse => {
                                    messageList.forceActiveFocus()
                                    console.info("mailo: click row", msgDelegate.index,
                                                 "modifiers", mouse.modifiers,
                                                 "anchor", messageList.selectionAnchor)
                                    if (mouse.modifiers & Qt.ControlModifier) {
                                        messageList.toggleSelect(msgDelegate.index)
                                        return
                                    }
                                    if (mouse.modifiers & Qt.ShiftModifier) {
                                        const anchor = messageList.selectionAnchor >= 0
                                            ? messageList.selectionAnchor
                                            : (messageList.currentIndex >= 0
                                               ? messageList.currentIndex : msgDelegate.index)
                                        console.info("mailo: shift range", anchor, "->",
                                                     msgDelegate.index)
                                        messageList.selectRange(anchor, msgDelegate.index)
                                        return
                                    }
                                    messageList.selectSingle(msgDelegate.index)
                                    messageList.currentIndex = msgDelegate.index
                                    // Clicks are deliberate — skip the key-repeat debounce.
                                    fetchDebounce.stop()
                                    // A draft is resumed, not read. Armed only
                                    // by a real click, so arrow-keying through
                                    // Drafts does not open a composer per row.
                                    root.draftEditPending = Mail.viewingDrafts
                                    Mail.fetchMessage(msgDelegate.index)
                                }
                                // Double-click: the message in its own window.
                                // The first click of the pair has already
                                // selected and fetched it, so this usually
                                // detaches instantly from the reading pane.
                                // Drafts open in the composer instead (the
                                // single-click path above), never in a window.
                                onDoubleClicked: mouse => {
                                    if (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                                        return
                                    if (Mail.viewingDrafts)
                                        return
                                    Mail.openMessageInWindow(msgDelegate.index)
                                }
                            }

                            contentItem: Row {
                                Repeater {
                                    model: messagePane.columns
                                    delegate: Item {
                                        id: rowCell
                                        required property string colId
                                        required property real weight

                                        width: messagePane.columnWidth(weight, columnHeader.width)
                                        height: root.listRowHeight

                                        Kirigami.Icon { // paperclip / calendar-invite marker
                                            visible: rowCell.colId === "attach"
                                                     && msgDelegate.hasAttachment
                                            anchors.centerIn: parent
                                            source: msgDelegate.calendarAttachment
                                                    ? "view-calendar" : "mail-attachment"
                                            width: Kirigami.Units.iconSizes.small
                                            height: width
                                            opacity: 0.7
                                        }
                                        QQC2.Label { // SPF/DKIM/DMARC failure marker
                                            id: authMark
                                            visible: rowCell.colId === "subject"
                                                     && msgDelegate.suspicious
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: "!"
                                            color: Kirigami.Theme.negativeTextColor
                                            font.bold: true
                                            QQC2.ToolTip.text: "Sender authentication failed:\n"
                                                               + msgDelegate.authInfo
                                            QQC2.ToolTip.visible: authHover.hovered
                                            HoverHandler { id: authHover }
                                        }
                                        QQC2.Label {
                                            visible: rowCell.colId !== "attach"
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: authMark.visible
                                                ? authMark.width + Kirigami.Units.smallSpacing * 2
                                                : Kirigami.Units.smallSpacing
                                            anchors.rightMargin: Kirigami.Units.smallSpacing
                                            text: rowCell.colId === "subject" ? msgDelegate.subject
                                                : rowCell.colId === "from" ? msgDelegate.from
                                                : rowCell.colId === "date" ? msgDelegate.date : ""
                                            elide: Text.ElideRight
                                            font.bold: rowCell.colId === "subject" && !msgDelegate.seen
                                            // Secondary columns are dimmed to ≥7:1 (AAA) on
                                            // normal rows, but shown at full opacity when the
                                            // row is highlighted — the theme's selection color
                                            // is already low-contrast, so dimming there would
                                            // push it further below AA.
                                            opacity: (rowCell.colId === "subject"
                                                      || msgDelegate.highlighted) ? 1 : 0.8
                                            // A marked, selected row shows the
                                            // mark as its TEXT color (the tint
                                            // background is suppressed under
                                            // selection). When not selected the
                                            // mark lives in the background tint,
                                            // so text uses the normal theme color.
                                            color: (msgDelegate.highlighted && msgDelegate.markColor !== "")
                                                   ? msgDelegate.markColor
                                                   : msgDelegate.highlighted
                                                     ? Kirigami.Theme.highlightedTextColor
                                                     : Kirigami.Theme.textColor
                                        }
                                    }
                                }
                            }
                        }

                        Kirigami.PlaceholderMessage {
                            anchors.centerIn: parent
                            width: parent.width - Kirigami.Units.gridUnit * 4
                            visible: messageList.count === 0
                            text: Mail.connected ? "No messages" : "Not connected"
                            icon.name: "mail-folder-inbox"
                        }
                    }
                }
            }

            // Viewer pane — bottom half of the right side
            MessageViewer {
                id: viewer
                context: Mail.readingContext
                ui: uiSettings
                QQC2.SplitView.fillHeight: true
                QQC2.SplitView.minimumHeight: 160
                onReplyRequested: replyAll => composeSheet().openReply(Mail.replyData(replyAll))
                onForwardRequested: composeSheet().openForward(Mail.forwardData())
            }
            }
        }
    }
}
