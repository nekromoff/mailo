import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Mailo.Core

Kirigami.ApplicationWindow {
    id: root
    title: "Mailo"
    width: 1200
    height: 760

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
    }

    // Mail-list row height from the density setting
    readonly property real listRowHeight:
        Kirigami.Units.gridUnit * [1.15, 1.4, 1.9][uiSettings.rowDensity]
    readonly property color panelColor: uiSettings.bgColor !== ""
        ? uiSettings.bgColor : Kirigami.Theme.backgroundColor

    Component.onCompleted: {
        if (Mail.hasAccount)
            Mail.connectAccount()
        else
            accountSheet.open()
    }

    Connections {
        target: Mail
        function onErrorOccurred(message) {
            root.showPassiveNotification(message, "long")
        }
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

    AccountSheet {
        id: accountSheet
        ui: uiSettings
    }

    ComposeSheet {
        id: composeSheet
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
                Layout.fillWidth: true
                text: Mail.statusText
                elide: Text.ElideRight
                opacity: 0.7
            }
            QQC2.ToolButton {
                icon.name: "mail-message-new"
                enabled: Mail.hasAccount
                onClicked: composeSheet.openNew()
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
                onClicked: accountSheet.open()
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
                                        QQC2.Label {
                                            Layout.fillWidth: true
                                            text: accountSection.modelData
                                            font.bold: true
                                            color: accountSection.isCurrent
                                                   ? Kirigami.Theme.highlightColor
                                                   : Kirigami.Theme.textColor
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

                                    property bool live: accountSection.isCurrent
                                    onLiveChanged: {
                                        if (live)
                                            folderPane.folderListView = folderList
                                    }
                                    Component.onCompleted: {
                                        if (live)
                                            folderPane.folderListView = folderList
                                    }

                                    function openCurrent() {
                                        const item = itemAtIndex(currentIndex)
                                        if (item && item.selectable) {
                                            messageList.currentIndex = -1
                                            messageList.clearSelection()
                                            Mail.openFolder(item.mailBox)
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
                                    onCurrentIndexChanged: {
                                        if (currentIndex >= 0)
                                            folderOpenDebounce.restart()
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
                                        icon.color: Qt.alpha(Kirigami.Theme.textColor, 0.45)
                                        highlighted: folderList.currentIndex === index
                                        onClicked: {
                                            if (!selectable) { // container-only folder: toggle instead
                                                Mail.folderModel.toggleExpanded(index)
                                                return
                                            }
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
                                            icon.color: Qt.alpha(Kirigami.Theme.textColor, 0.45)
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
                                        opacity: 0.5
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
                        function selectSingle(i) {
                            const s = {}
                            s[i] = true
                            selectedSet = s
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
                        Keys.onDeletePressed: requestDelete()

                        // Row indexes shift on re-sort/search — selections
                        // would silently point at the wrong messages.
                        Connections {
                            target: Mail.messageModel
                            function onModelReset() { messageList.clearSelection() }
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
                            required property int index

                            width: messageList.width
                            // Row height comes from the density setting alone;
                            // fixed slim padding keeps the density steps even.
                            topPadding: 1
                            bottomPadding: 1
                            highlighted: messageList.currentIndex === index
                                         || messageList.isSelected(index)
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
                                            opacity: rowCell.colId === "subject" ? 1 : 0.7
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
                onReplyRequested: replyAll => composeSheet.openReply(Mail.replyData(replyAll))
                onForwardRequested: composeSheet.openForward(Mail.forwardData())
            }
            }
        }
    }
}
