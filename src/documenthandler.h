// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextListFormat>

/**
 * Formatting backend for the compose editor. QML TextArea has no API for
 * lists or programmatic character formatting, so this wraps QTextCursor.
 */
class DocumentHandler : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorPositionChanged)
    Q_PROPERTY(int selectionStart READ selectionStart WRITE setSelectionStart NOTIFY selectionStartChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd WRITE setSelectionEnd NOTIFY selectionEndChanged)
    Q_PROPERTY(bool bold READ bold WRITE setBold NOTIFY formatChanged)
    Q_PROPERTY(bool italic READ italic WRITE setItalic NOTIFY formatChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY formatChanged)

public:
    using QObject::QObject;

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);

    int cursorPosition() const { return m_cursorPosition; }
    void setCursorPosition(int position);
    int selectionStart() const { return m_selectionStart; }
    void setSelectionStart(int position);
    int selectionEnd() const { return m_selectionEnd; }
    void setSelectionEnd(int position);

    bool bold() const;
    void setBold(bool bold);
    bool italic() const;
    void setItalic(bool italic);
    int fontSize() const;
    void setFontSize(int size);

    Q_INVOKABLE void toggleBulletList();
    Q_INVOKABLE void toggleOrderedList();

    /// Turns a "-" or "*" written on its own at the start of a block into a
    /// bulleted list, swallowing the space that triggered it. Returns true when
    /// it did, so the caller can eat the key press.
    Q_INVOKABLE bool startBulletList();

    /// Same for a number written at the start of a block, triggered by the
    /// terminator being typed after it ("." or ")") rather than by a following
    /// space. The terminator is kept as the list's number suffix.
    Q_INVOKABLE bool startOrderedList(const QString &terminator);

    /// Leaves the list when Return is pressed on an empty item — the second of
    /// the two Enters that ends a list. Returns true when it did.
    Q_INVOKABLE bool leaveEmptyListItem();

    /// Tab and Shift+Tab inside a list: one level in, one level out. False when
    /// the cursor is not in a list, leaving Tab to move focus as usual.
    Q_INVOKABLE bool indentListItem();
    Q_INVOKABLE bool outdentListItem();

    /// Backspace at the start of a list item: out one level, and out of the
    /// list itself from the outermost one.
    Q_INVOKABLE bool outdentAtBlockStart();

    /// Ctrl+Shift+V: inserts the clipboard as unformatted text, taking the
    /// formatting of the text it lands in rather than dragging the source
    /// document's fonts and colors into the message. False when the clipboard
    /// holds no text, so the caller can leave the key press alone.
    Q_INVOKABLE bool pastePlainText();

Q_SIGNALS:
    void documentChanged();
    void cursorPositionChanged();
    void selectionStartChanged();
    void selectionEndChanged();
    void formatChanged();

private:
    QTextCursor textCursor() const;
    bool atBlockStart(const QTextCursor &cursor) const;
    void mergeFormat(const QTextCharFormat &format);
    void toggleList(int listStyle);
    void applyMarkerList(QTextCursor &cursor, const QTextListFormat &listFormat);
    bool changeListIndent(int delta);

    QQuickTextDocument *m_document = nullptr;
    int m_cursorPosition = -1;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
};
