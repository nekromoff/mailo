// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One value in the envelope header — From, To, Cc, Subject, Date.
//
// A plain Label cannot be selected: Qt only implements selection on TextEdit,
// which is what Kirigami.SelectableLabel wraps. TextEdit in turn cannot elide,
// so the single-line header layout is kept by clipping instead, and the full
// value is offered two other ways: hovering shows it, and the right-click menu
// (Select All → Copy) reaches the clipped-off part that the cursor cannot.
Kirigami.SelectableLabel {
    id: value

    Layout.fillWidth: true
    // Ask for no width of our own. Preferring the text's width would let one
    // long subject or a large recipient list widen the whole window; instead
    // the field takes whatever the row has left over.
    Layout.preferredWidth: 0

    wrapMode: Text.NoWrap
    clip: true

    // True when the clip is hiding part of the value.
    readonly property bool overflowing: value.contentWidth > value.width

    HoverHandler { id: valueHover }
    HoverToolTip {
        hover: valueHover
        // Only worth a tooltip when there is something to reveal.
        text: value.overflowing ? value.text : ""
    }
}
