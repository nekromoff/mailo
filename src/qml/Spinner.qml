// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtQuick
import org.kde.kirigami as Kirigami

/// A plain rotating arc — a spinner.
///
/// Not QQC2.BusyIndicator: the org.kde.desktop style draws that as the
/// "process-working-symbolic" icon under a rotation animator, and in Breeze
/// that icon is a cog. Spinning gears read as "configuring something", not as
/// "waiting for the network", so this draws the arc itself.
///
/// The arc is painted once and the whole canvas is rotated, so the animation
/// costs a transform per frame rather than a repaint.
Item {
    id: root

    /// Diameter. Defaults to the size of a small icon, which is what makes it
    /// sit correctly next to a line of text.
    property int size: Kirigami.Units.iconSizes.small
    property bool running: true
    /// Matches the blue arc the main window and the send button already draw
    /// (Main.qml, ComposeSheet.qml) — this component exists to be the shared
    /// version of those, so it has to look identical to them.
    property color color: Kirigami.Theme.highlightColor

    implicitWidth: size
    implicitHeight: size
    visible: running

    Canvas {
        id: arc
        anchors.fill: parent

        // Three quarters of a circle, open at the end — the same figure the
        // other two spinners draw, scaled to whatever size is asked for.
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const lw = Math.max(1.5, root.size / 5)
            const r = width / 2 - lw / 2
            if (r <= 0)
                return
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, r, 0, Math.PI * 1.5)
            ctx.strokeStyle = root.color
            ctx.lineWidth = lw
            ctx.lineCap = "round"
            ctx.stroke()
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        RotationAnimator on rotation {
            running: root.running && root.visible
            loops: Animation.Infinite
            from: 0
            to: 360
            duration: 900
        }
    }

    onColorChanged: arc.requestPaint()
    onSizeChanged: arc.requestPaint()
}
