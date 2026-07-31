// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <QTextCursor>

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

Q_SIGNALS:
    void documentChanged();
    void cursorPositionChanged();
    void selectionStartChanged();
    void selectionEndChanged();
    void formatChanged();

private:
    QTextCursor textCursor() const;
    void mergeFormat(const QTextCharFormat &format);
    void toggleList(int listStyle);

    QQuickTextDocument *m_document = nullptr;
    int m_cursorPosition = -1;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
};
