#pragma once

#include <QAbstractListModel>
#include <QDateTime>
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
        CalendarRole
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
    qint64 uidAt(int row) const;
    void markSeen(int row);
    /// Refines a message's attachment kind in place (body-derived knowledge).
    void setAttachKind(qint64 uid, int kind);
    /// Drops the given uids from the model (visible and hidden lists).
    void removeByUids(const QList<qint64> &uids);

    /// Show only rows whose subject or sender matches; empty pattern clears.
    void applyFilter(const QRegularExpression &pattern);
    bool hasFilter() const { return m_filter.isValid() && !m_filter.pattern().isEmpty(); }

    Q_INVOKABLE void sortBy(int column, bool descending);

private:
    void rebuildVisible();
    void sortList(QList<Header> &list) const;
    bool lessThan(const Header &a, const Header &b) const;
    bool matchesFilter(const Header &h) const;

    QString m_dateFormat = QStringLiteral("yyyy-MM-dd");
    QList<Header> m_headers; ///< visible (possibly filtered) rows
    QList<Header> m_all;     ///< everything fetched for the folder
    QRegularExpression m_filter;
    SortColumn m_sortColumn = SortColumn::Date;
    bool m_sortDescending = true;
};
