// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "documenthandler.h"

#include <QQuickTextDocument>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>

void DocumentHandler::setDocument(QQuickTextDocument *document)
{
    if (m_document == document)
        return;
    m_document = document;
    Q_EMIT documentChanged();
    Q_EMIT formatChanged();
}

QTextCursor DocumentHandler::textCursor() const
{
    if (!m_document)
        return {};
    QTextCursor cursor(m_document->textDocument());
    if (m_selectionStart != m_selectionEnd) {
        cursor.setPosition(m_selectionStart);
        cursor.setPosition(m_selectionEnd, QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(m_cursorPosition >= 0 ? m_cursorPosition : 0);
    }
    return cursor;
}

void DocumentHandler::mergeFormat(const QTextCharFormat &format)
{
    QTextCursor cursor = textCursor();
    if (cursor.isNull())
        return;
    if (!cursor.hasSelection())
        cursor.select(QTextCursor::WordUnderCursor);
    cursor.mergeCharFormat(format);
    Q_EMIT formatChanged();
}

void DocumentHandler::setCursorPosition(int position)
{
    if (m_cursorPosition == position)
        return;
    m_cursorPosition = position;
    Q_EMIT cursorPositionChanged();
    Q_EMIT formatChanged();
}

void DocumentHandler::setSelectionStart(int position)
{
    if (m_selectionStart == position)
        return;
    m_selectionStart = position;
    Q_EMIT selectionStartChanged();
}

void DocumentHandler::setSelectionEnd(int position)
{
    if (m_selectionEnd == position)
        return;
    m_selectionEnd = position;
    Q_EMIT selectionEndChanged();
}

bool DocumentHandler::bold() const
{
    return textCursor().charFormat().fontWeight() >= QFont::Bold;
}

void DocumentHandler::setBold(bool bold)
{
    QTextCharFormat format;
    format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
    mergeFormat(format);
}

bool DocumentHandler::italic() const
{
    return textCursor().charFormat().fontItalic();
}

void DocumentHandler::setItalic(bool italic)
{
    QTextCharFormat format;
    format.setFontItalic(italic);
    mergeFormat(format);
}

int DocumentHandler::fontSize() const
{
    const int size = int(textCursor().charFormat().font().pointSizeF());
    return size > 0 ? size : 11;
}

void DocumentHandler::setFontSize(int size)
{
    if (size < 6 || size > 72)
        return;
    QTextCharFormat format;
    format.setFontPointSize(size);
    mergeFormat(format);
}

void DocumentHandler::toggleList(int listStyle)
{
    QTextCursor cursor = textCursor();
    if (cursor.isNull())
        return;
    cursor.beginEditBlock();
    if (QTextList *list = cursor.currentList();
        list && list->format().style() == listStyle) {
        // Already this list type → remove list formatting from the block.
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setIndent(0);
        blockFormat.setObjectIndex(-1);
        cursor.setBlockFormat(blockFormat);
    } else {
        QTextListFormat listFormat;
        listFormat.setStyle(QTextListFormat::Style(listStyle));
        cursor.createList(listFormat);
    }
    cursor.endEditBlock();
    Q_EMIT formatChanged();
}

void DocumentHandler::toggleBulletList()
{
    toggleList(QTextListFormat::ListDisc);
}

void DocumentHandler::toggleOrderedList()
{
    toggleList(QTextListFormat::ListDecimal);
}
