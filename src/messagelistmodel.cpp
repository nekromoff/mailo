// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "messagelistmodel.h"

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_headers.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_headers.size())
        return {};
    const Header &h = m_headers.at(index.row());
    switch (role) {
    case SubjectRole:
        return h.subject.isEmpty() ? QStringLiteral("(no subject)") : h.subject;
    case FromRole:
        return h.from;
    case DateRole: {
        const QDateTime local = h.date.toLocalTime();
        if (local.date() == QDate::currentDate())
            return local.toString(QStringLiteral("hh:mm"));
        return local.toString(m_dateFormat + QStringLiteral(" hh:mm"));
    }
    case UidRole:
        return h.uid;
    case SeenRole:
        return h.seen;
    case SuspiciousRole:
        return h.suspicious;
    case AuthInfoRole:
        return h.authInfo;
    case AttachmentRole:
        return h.attachKind != NoAttachment;
    case CalendarRole:
        return h.attachKind == CalendarAttachment;
    case ColorLabelRole:
        return h.colorLabel;
    }
    return {};
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {
        {SubjectRole, "subject"},
        {FromRole, "from"},
        {DateRole, "date"},
        {UidRole, "uid"},
        {SeenRole, "seen"},
        {SuspiciousRole, "suspicious"},
        {AuthInfoRole, "authInfo"},
        {AttachmentRole, "hasAttachment"},
        {CalendarRole, "calendarAttachment"},
        {ColorLabelRole, "colorLabel"},
    };
}

void MessageListModel::setDateFormat(const QString &format)
{
    if (m_dateFormat == format)
        return;
    m_dateFormat = format;
    if (!m_headers.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_headers.size() - 1), {DateRole});
}

void MessageListModel::setHeaders(QList<Header> headers)
{
    m_all = std::move(headers);
    sortList(m_all);
    rebuildVisible();
}

int MessageListModel::appendHeaders(const QList<Header> &headers)
{
    if (headers.isEmpty())
        return 0;
    int added = 0;
    // Sorted per-row inserts instead of a model reset, so the ListView
    // keeps its scroll position when older messages arrive.
    QSet<qint64> known;
    known.reserve(m_all.size());
    for (const Header &h : std::as_const(m_all))
        known.insert(h.uid);

    const auto cmp = [this](const Header &a, const Header &b) { return lessThan(a, b); };
    for (const Header &h : headers) {
        if (known.contains(h.uid)) {
            // Already listed (usually from the disk cache): refresh the row so
            // server-derived fields (seen, attachment, auth verdict) update.
            Header merged = h;
            for (Header &existing : m_all) {
                if (existing.uid == h.uid) {
                    // A head-only refresh only knows generic/none — keep the
                    // refined kind (calendar invite) learned from the body.
                    if (existing.attachKind > GenericAttachment
                        && h.attachKind == GenericAttachment)
                        merged.attachKind = existing.attachKind;
                    // A locally-read message stays read: the server refresh
                    // may predate our \Seen write-back landing there.
                    if (existing.seen)
                        merged.seen = true;
                    // The color mark is local-only — the server never knows it.
                    if (existing.colorLabel != 0)
                        merged.colorLabel = existing.colorLabel;
                    existing = merged;
                    break;
                }
            }
            for (int row = 0; row < m_headers.size(); ++row) {
                if (m_headers.at(row).uid == h.uid) {
                    m_headers[row] = merged;
                    const QModelIndex idx = index(row);
                    Q_EMIT dataChanged(idx, idx);
                    break;
                }
            }
            continue;
        }
        known.insert(h.uid);
        ++added;
        m_all.insert(std::upper_bound(m_all.begin(), m_all.end(), h, cmp) - m_all.begin(), h);
        if ((hasFilter() || m_colorFilter != 0) && !matchesFilter(h))
            continue;
        const int row = int(std::upper_bound(m_headers.begin(), m_headers.end(), h, cmp)
                            - m_headers.begin());
        beginInsertRows({}, row, row);
        m_headers.insert(row, h);
        endInsertRows();
    }
    return added;
}

void MessageListModel::clear()
{
    beginResetModel();
    m_headers.clear();
    m_all.clear();
    m_filter = QRegularExpression();
    endResetModel();
}

void MessageListModel::applyFilter(const QRegularExpression &pattern)
{
    m_filter = pattern;
    rebuildVisible();
}

void MessageListModel::sortBy(int column, bool descending)
{
    m_sortColumn = SortColumn(column);
    m_sortDescending = descending;
    sortList(m_all);
    rebuildVisible();
}

bool MessageListModel::lessThan(const Header &a, const Header &b) const
{
    int c;
    switch (m_sortColumn) {
    case SortColumn::From:
        c = QString::compare(a.from, b.from, Qt::CaseInsensitive);
        break;
    case SortColumn::Subject:
        c = QString::compare(a.subject, b.subject, Qt::CaseInsensitive);
        break;
    case SortColumn::Attachment:
        // Ties fall back to date so the groups stay chronological.
        c = int(a.attachKind != NoAttachment) - int(b.attachKind != NoAttachment);
        if (c == 0)
            c = a.date < b.date ? 1 : (b.date < a.date ? -1 : 0);
        break;
    default:
        c = a.date < b.date ? -1 : (b.date < a.date ? 1 : 0);
        break;
    }
    return m_sortDescending ? c > 0 : c < 0;
}

bool MessageListModel::matchesFilter(const Header &h) const
{
    if (m_colorFilter != 0 && h.colorLabel != m_colorFilter)
        return false;
    if (!hasFilter())
        return true;
    return m_filter.match(h.subject).hasMatch() || m_filter.match(h.from).hasMatch();
}

void MessageListModel::setColorFilter(int color)
{
    if (m_colorFilter == color)
        return;
    m_colorFilter = color;
    rebuildVisible();
}

void MessageListModel::sortList(QList<Header> &list) const
{
    std::stable_sort(list.begin(), list.end(),
                     [this](const Header &a, const Header &b) { return lessThan(a, b); });
}

void MessageListModel::rebuildVisible()
{
    beginResetModel();
    if (hasFilter() || m_colorFilter != 0) {
        m_headers.clear();
        for (const Header &h : std::as_const(m_all)) {
            if (matchesFilter(h))
                m_headers.append(h);
        }
    } else {
        m_headers = m_all;
    }
    endResetModel();
}

qint64 MessageListModel::uidAt(int row) const
{
    return (row >= 0 && row < m_headers.size()) ? m_headers.at(row).uid : -1;
}

bool MessageListModel::seenAt(int row) const
{
    return row >= 0 && row < m_headers.size() && m_headers.at(row).seen;
}

void MessageListModel::removeByUids(const QList<qint64> &uids)
{
    const QSet<qint64> gone(uids.begin(), uids.end());
    for (int i = m_headers.size() - 1; i >= 0; --i) {
        if (gone.contains(m_headers.at(i).uid)) {
            beginRemoveRows({}, i, i);
            m_headers.removeAt(i);
            endRemoveRows();
        }
    }
    m_all.removeIf([&gone](const Header &h) { return gone.contains(h.uid); });
}

void MessageListModel::setAttachKind(qint64 uid, int kind)
{
    for (Header &h : m_all) {
        if (h.uid == uid) {
            h.attachKind = kind;
            break;
        }
    }
    for (int row = 0; row < m_headers.size(); ++row) {
        if (m_headers.at(row).uid == uid) {
            if (m_headers[row].attachKind != kind) {
                m_headers[row].attachKind = kind;
                const QModelIndex idx = index(row);
                Q_EMIT dataChanged(idx, idx, {AttachmentRole, CalendarRole});
            }
            break;
        }
    }
}

int MessageListModel::colorLabelAt(int row) const
{
    return (row >= 0 && row < m_headers.size()) ? m_headers.at(row).colorLabel : 0;
}

void MessageListModel::setColorLabel(qint64 uid, int color)
{
    const Header *updated = nullptr;
    for (Header &h : m_all) {
        if (h.uid == uid) {
            h.colorLabel = color;
            updated = &h;
            break;
        }
    }
    if (!updated)
        return;

    int row = -1;
    for (int i = 0; i < m_headers.size(); ++i) {
        if (m_headers.at(i).uid == uid) {
            row = i;
            break;
        }
    }
    // The change can move the row into or out of an active color filter —
    // insert/remove it instead of only repainting in place.
    const bool matches = matchesFilter(*updated);
    if (row >= 0 && !matches) {
        beginRemoveRows({}, row, row);
        m_headers.removeAt(row);
        endRemoveRows();
    } else if (row < 0 && matches) {
        const auto cmp = [this](const Header &a, const Header &b) { return lessThan(a, b); };
        const int at = int(std::upper_bound(m_headers.begin(), m_headers.end(), *updated, cmp)
                           - m_headers.begin());
        beginInsertRows({}, at, at);
        m_headers.insert(at, *updated);
        endInsertRows();
    } else if (row >= 0 && m_headers.at(row).colorLabel != color) {
        m_headers[row].colorLabel = color;
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {ColorLabelRole});
    }
}

void MessageListModel::markSeen(int row)
{
    if (row < 0 || row >= m_headers.size())
        return;
    m_headers[row].seen = true;
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {SeenRole});
}
