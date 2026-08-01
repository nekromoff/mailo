// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QSet>

/// Message headers of the currently selected folder, newest first.
class MessageListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        SubjectRole = Qt::UserRole + 1,
        FromRole,
        DateRole,
        UidRole,
        SeenRole,
        SuspiciousRole,
        AuthInfoRole,
        AttachmentRole,
        CalendarRole,
        ColorLabelRole
    };

    /// Attachment kinds carried in Header::attachKind.
    enum AttachKind { NoAttachment = 0, GenericAttachment = 1, CalendarAttachment = 2 };

    struct Header {
        qint64 uid = -1;
        QString subject;
        QString from;
        QDateTime date;
        bool seen = false;
        bool suspicious = false; ///< SPF/DKIM/DMARC failure reported by our server
        QString authInfo;        ///< raw Authentication-Results header
        int attachKind = NoAttachment; ///< 1 = multipart/mixed head, 2 = all-.ics attachments
        int colorLabel = 0;      ///< local color-scale mark (0 = none, 1..5)
        /// RFC 5322 Message-ID with the angle brackets stripped. Stable across
        /// folders and UIDVALIDITY resets, unlike uid.
        QString msgid;

        // Sort keys, derived from the fields above by primeKeys() when the
        // header enters the model. Producers do not fill them. They exist so a
        // comparison costs an integer compare or a plain QString compare,
        // instead of a QDateTime compare (timezone-aware, local-spec) or a
        // case-insensitive compare that re-folds both strings every time — on
        // a list of 100k rows that is the difference between a sort the user
        // does not notice and one that freezes the GUI thread.
        qint64 dateSecs = 0;  ///< date.toSecsSinceEpoch(), 0 when invalid
        QString fromKey;      ///< case-folded from
        QString subjectKey;   ///< case-folded subject
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    enum class SortColumn { Date = 0, From = 1, Subject = 2, Attachment = 3 };

    /// Date pattern (Qt format string) used for non-today rows; today's rows
    /// always show only the time.
    void setDateFormat(const QString &format);

    void setHeaders(QList<Header> headers);
    /// Returns the number of rows actually inserted (uid duplicates skipped).
    int appendHeaders(const QList<Header> &headers);
    void clear();
    Q_INVOKABLE qint64 uidAt(int row) const;
    /// Visible row showing \a uid, or -1 when it is not in the model. Lets the
    /// view re-find the message the user picked after a reset renumbers rows.
    Q_INVOKABLE int rowForUid(qint64 uid) const;
    bool seenAt(int row) const;
    void markSeen(int row);
    /// Refines a message's attachment kind in place (body-derived knowledge).
    void setAttachKind(qint64 uid, int kind);
    int colorLabelAt(int row) const;
    void setColorLabel(qint64 uid, int color);
    /// Drops the given uids from the model (visible and hidden lists).
    void removeByUids(const QList<qint64> &uids);

    /// Show only rows whose subject or sender matches; empty pattern clears.
    void applyFilter(const QRegularExpression &pattern);
    bool hasFilter() const { return m_filter.isValid() && !m_filter.pattern().isEmpty(); }

    Q_INVOKABLE void sortBy(int column, bool descending);
    /// Quick filter: show only rows carrying this color mark (0 = off).
    Q_INVOKABLE void setColorFilter(int color);

private:
    /// Fills the derived sort keys of a header entering the model.
    static void primeKeys(Header &h);
    /// Recomputes m_rows (sort + filter) inside a model reset.
    void rebuildVisible();
    /// Re-sorts m_rows in place and reports it as a layout change, so the view
    /// keeps its scroll position and its selection.
    void resortVisible();
    /// Rebuilds the uid → m_all index map after m_all is replaced or spliced.
    void reindex();
    /// Visible row showing the m_all entry \a allIndex, or -1 when filtered out.
    int visibleRowOf(int allIndex) const { return m_rows.indexOf(allIndex); }
    bool lessThan(const Header &a, const Header &b) const;
    bool matchesFilter(const Header &h) const;

    QString m_dateFormat = QStringLiteral("yyyy-MM-dd");
    /// Everything fetched for the folder, in arrival order. Rows are only ever
    /// appended here, so the indices held in m_rows and m_byUid stay valid;
    /// removeByUids() is the one exception and rebuilds both.
    QList<Header> m_all;
    /// Visible rows: indices into m_all, in sort order, filter applied. The
    /// visible list is a permutation and not a second copy of the headers —
    /// copying them meant every insert detached the shared list (a deep copy
    /// of the whole folder) and left two copies of state to keep in step.
    QList<int> m_rows;
    QHash<qint64, int> m_byUid; ///< uid → index into m_all
    QRegularExpression m_filter;
    int m_colorFilter = 0;
    SortColumn m_sortColumn = SortColumn::Date;
    bool m_sortDescending = true;
};
