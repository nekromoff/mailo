// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Mailo.Core

/// A message opened in its own top-level window (double-click in the list).
/// Owns a MessageContext, so it keeps showing — and serving inline images,
/// attachments, Reply/Forward — whatever the main window moves on to.
Window {
    id: win
    // Normal decorated window: the system title bar supplies close, minimize
    // and maximize, and the taskbar gets its own entry.
    flags: Qt.Window
    transientParent: null

    /// The MessageContext this window owns; released when the window closes.
    property var context: null

    /// The uiSettings object from Main.qml (for the bgColor override).
    property var ui: null

    signal replyRequested(bool replyAll)
    signal forwardRequested()

    title: context && context.subject.length > 0 ? context.subject : "Message"

    // Message windows share one remembered size (and maximized state) —
    // whatever the last closed one was. Position is deliberately left to the
    // window manager: restoring it would stack every window on the same spot.
    Settings {
        id: windowState
        category: "messageWindow"
        property int width: 860
        property int height: 700
        property bool maximized: false
    }
    width: windowState.width
    height: windowState.height
    minimumWidth: 400
    minimumHeight: 300

    function present() {
        if (windowState.maximized)
            showMaximized()
        else
            show()
        raise()
        requestActivate()
    }

    // Same panel treatment as the compose window: chrome-gray Window color
    // set, with the user's bgColor override winning.
    color: ui && ui.bgColor !== "" ? ui.bgColor
                                   : messageViewer.Kirigami.Theme.backgroundColor

    Shortcut {
        sequence: "Esc"
        // While the find bar is open Esc belongs to it — closing the whole
        // window on a dismissed search would be a nasty surprise.
        enabled: !messageViewer.findActive
        onActivated: win.close()
    }

    MessageViewer {
        id: messageViewer
        anchors.fill: parent
        context: win.context
        ui: win.ui
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        Kirigami.Theme.inherit: false
        onReplyRequested: replyAll => win.replyRequested(replyAll)
        onForwardRequested: win.forwardRequested()
    }

    onClosing: {
        windowState.maximized = win.visibility === Window.Maximized
        if (win.visibility === Window.Windowed) {
            windowState.width = win.width
            windowState.height = win.height
        }
        // Free the context (scheme-handler slot, KMime message) and this
        // window's WebEngine view. Deferred: tearing the window down from
        // inside its own closing handler is asking for trouble.
        Qt.callLater(function() {
            if (win.context)
                win.context.release()
            win.destroy()
        })
    }
}
