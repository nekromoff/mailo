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
    Settings {
        id: windowSettings
        category: "window"
        property int width: 1200
        property int height: 760
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
        // Keep the last windowed size — maximized dimensions would make
        // un-maximizing on the next run a no-op.
        if (root.visibility === Window.Windowed) {
            windowSettings.width = root.width
            windowSettings.height = root.height
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

    // The definable shortcuts only act while the message list has focus —
    // they never conflict with typing in search, compose or settings fields.
    function matchesShortcut(event, seq) {
        if (!seq)
            return false
        const parts = seq.split("+")
        let mods = 0
        for (let i = 0; i < parts.length - 1; i++) {
            const m = parts[i].trim().toLowerCase()
            if (m === "ctrl") mods |= Qt.ControlModifier
            else if (m === "shift") mods |= Qt.ShiftModifier
            else if (m === "alt") mods |= Qt.AltModifier
            else if (m === "meta") mods |= Qt.MetaModifier
        }
        if ((event.modifiers & ~Qt.KeypadModifier) !== mods)
            return false
        const keyName = parts[parts.length - 1].trim().toLowerCase()
        const named = {
            "del": Qt.Key_Delete, "delete": Qt.Key_Delete,
            "backspace": Qt.Key_Backspace, "space": Qt.Key_Space,
            "ins": Qt.Key_Insert, "insert": Qt.Key_Insert,
            "home": Qt.Key_Home, "end": Qt.Key_End
        }
        if (keyName in named)
            return event.key === named[keyName]
        if (/^f\d{1,2}$/.test(keyName))
            return event.key === Qt.Key_F1 + parseInt(keyName.substring(1)) - 1
        if (keyName.length === 1)
            return event.key === keyName.toUpperCase().charCodeAt(0)
        return false
    }

    function handleMailShortcut(event) {
        if (matchesShortcut(event, uiSettings.shortcutDelete))
            messageList.requestDelete()
        else if (matchesShortcut(event, uiSettings.shortcutJunk))
            messageList.requestJunk()
        else if (matchesShortcut(event, uiSettings.shortcutCompose) && Mail.hasAccount)
            composeSheet().openNew()
        else if (matchesShortcut(event, uiSettings.shortcutReply) && viewer.hasMessage)
            composeSheet().openReply(Mail.replyData(false))
        else if (matchesShortcut(event, uiSettings.shortcutForward) && viewer.hasMessage)
            composeSheet().openForward(Mail.forwardData())
        else if (matchesShortcut(event, uiSettings.shortcutSelect))
            messageList.toggleSelectAndAdvance()
        else if (!handleScaleShortcut(event))
            return
        event.accepted = true
    }

    // Color-scale shortcuts: mark the selection with the slot's color, or
    // clear the mark when the slot has no color defined.
    function handleScaleShortcut(event) {
        for (let i = 1; i <= 5; i++) {
            const seq = uiSettings["scaleKey" + i]
            if (seq !== "" && matchesShortcut(event, seq)) {
                Mail.markMessageColor(messageList.selectedIndexes(),
                                      scaleColorOf(i) !== "" ? i : 0)
                return true
            }
        }
        return false
    }

    Component.onCompleted: {
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
        function onMessageLoaded(subject, from, to, cc, date, bodyUrl, authInfo) {
            viewer.showMessage(subject, from, to, cc, date, bodyUrl, authInfo)
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
    // Component + createObject, not a Loader: AccountSheet is a Dialog that
    // centers itself with anchors.centerIn: parent, so it must be parented to
    // the window's contentItem exactly as the old declarative form was. A
    // Loader would make its 0x0 self the parent and the dialog would size and
    // position against nothing.
    property var accountDialog: null
    Component {
        id: accountComponent
        AccountSheet { ui: uiSettings }
    }
    function accountSheet() {
        if (!accountDialog)
            accountDialog = accountComponent.createObject(root.contentItem)
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
                text: Mail.statusText
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
                QQC2.ToolTip.text: "Account settings"
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
                                        root.handleMailShortcut(event)
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
                                        onClicked: {
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
                                            messageList.clearSelection()
                                            Mail.openFolder(mailBox)
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
                        Keys.onPressed: event => root.handleMailShortcut(event)

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
                                // message. Re-anchor on the first row and fetch
                                // it explicitly.
                                if (messageList.count > 0) {
                                    messageList.currentIndex = 0
                                    fetchDebounce.restart()
                                } else {
                                    messageList.currentIndex = -1
                                }
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
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
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
                                    Mail.fetchMessage(msgDelegate.index)
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
                QQC2.SplitView.fillHeight: true
                QQC2.SplitView.minimumHeight: 160
                onReplyRequested: replyAll => composeSheet().openReply(Mail.replyData(replyAll))
                onForwardRequested: composeSheet().openForward(Mail.forwardData())
            }
            }
        }
    }
}
