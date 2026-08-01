// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// A header value that routinely outgrows its line — the recipient lists. It
// stays a single clipped line like every other field until it has something to
// hide, then offers a caret that wraps the whole list into view. Mail sent to
// thirty people should not push the message itself off the screen by default,
// but the recipients still have to be readable when they are what you came for.
RowLayout {
    id: field

    property alias text: value.text
    property bool expanded: false

    // Where the caret should sit, in this field's own coordinates. The header
    // reads as a column of values, so the caret belongs on that column's edge —
    // parked at the far right of the window it looks unrelated to the text it
    // belongs to. Ignored while nothing is folded away.
    property real caretX: -1

    Layout.fillWidth: true
    spacing: Kirigami.Units.smallSpacing

    SelectableValue {
        id: value

        // Give up the width right of the caret, but only once there is a caret:
        // a list that fits has no reason to stop short of the full row.
        Layout.maximumWidth: caret.visible && field.caretX > 0
            ? Math.max(field.caretX - field.spacing, Kirigami.Units.gridUnit * 6)
            : Number.POSITIVE_INFINITY
        // Stays at the top so the caret does not drift down the middle of a
        // recipient list several lines tall.
        Layout.alignment: Qt.AlignTop
        wrapMode: field.expanded ? Text.Wrap : Text.NoWrap
        clip: !field.expanded
        // A different message is a different recipient list; the reader did not
        // ask to see this one unfolded.
        onTextChanged: field.expanded = false
    }

    QQC2.Label {
        id: caret

        // Once expanded the text fits by definition, so the overflow test that
        // summoned the caret would immediately take it away again — keep it for
        // as long as it is the only way back.
        visible: value.overflowing || field.expanded
        Layout.alignment: Qt.AlignTop
        // Explicit glyphs: this has to read as a disclosure arrow at caption
        // size, which theme icons do not reliably do. Filled and oversized —
        // it is the only thing in the header that can be clicked, and a hairline
        // caret next to a wall of addresses simply does not get noticed.
        text: field.expanded ? "▼" : "▶"
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.15
        opacity: caretHover.hovered ? 1 : 0.85

        HoverHandler {
            id: caretHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: field.expanded = !field.expanded
        }
        HoverToolTip {
            hover: caretHover
            text: field.expanded ? "Show one line only" : "Show every recipient"
        }
    }
}
