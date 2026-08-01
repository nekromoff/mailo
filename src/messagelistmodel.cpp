// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "messagelistmodel.h"

#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>

namespace
{
/// Same contract as MailStore's SlowGuard: anything above one frame spent in
/// here is spent with the window frozen, so say so instead of letting it hide.
constexpr qint64 kSlowMs = 20;
}

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Header &h = m_all.at(m_rows.at(index.row()));
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

void MessageListModel::primeKeys(Header &h)
{
    h.dateSecs = h.date.isValid() ? h.date.toSecsSinceEpoch() : 0;
    h.fromKey = h.from.toCaseFolded();
    h.subjectKey = h.subject.toCaseFolded();
}

void MessageListModel::setDateFormat(const QString &format)
{
    if (m_dateFormat == format)
        return;
    m_dateFormat = format;
    if (!m_rows.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_rows.size() - 1), {DateRole});
}

void MessageListModel::setHeaders(QList<Header> headers)
{
    m_all = std::move(headers);
    for (Header &h : m_all)
        primeKeys(h);
    reindex();
    rebuildVisible();
}

int MessageListModel::appendHeaders(const QList<Header> &headers)
{
    if (headers.isEmpty())
        return 0;
    int added = 0;
    // Sorted per-row inserts instead of a model reset, so the ListView
    // keeps its scroll position when older messages arrive.
    const auto cmp = [this](int a, int b) { return lessThan(m_all.at(a), m_all.at(b)); };
    for (const Header &h : headers) {
        const auto known = m_byUid.constFind(h.uid);
        if (known != m_byUid.constEnd()) {
            // Already listed (usually from the disk cache): refresh the row so
            // server-derived fields (seen, attachment, auth verdict) update.
            Header &existing = m_all[known.value()];
            Header merged = h;
            // A head-only refresh only knows generic/none — keep the
            // refined kind (calendar invite) learned from the body.
            if (existing.attachKind > GenericAttachment && h.attachKind == GenericAttachment)
                merged.attachKind = existing.attachKind;
            // A locally-read message stays read: the server refresh
            // may predate our \Seen write-back landing there.
            if (existing.seen)
                merged.seen = true;
            // The color mark is local-only — the server never knows it.
            if (existing.colorLabel != 0)
                merged.colorLabel = existing.colorLabel;
            primeKeys(merged);
            existing = merged;
            const int row = visibleRowOf(known.value());
            if (row >= 0) {
                const QModelIndex idx = index(row);
                Q_EMIT dataChanged(idx, idx);
            }
            continue;
        }
        ++added;
        const int at = m_all.size();
        m_all.append(h);
        primeKeys(m_all[at]);
        m_byUid.insert(h.uid, at);
        if ((hasFilter() || m_colorFilter != 0) && !matchesFilter(m_all.at(at)))
            continue;
        const int row = int(std::upper_bound(m_rows.begin(), m_rows.end(), at, cmp)
                            - m_rows.begin());
        beginInsertRows({}, row, row);
        m_rows.insert(row, at);
        endInsertRows();
    }
    return added;
}

void MessageListModel::clear()
{
    beginResetModel();
    m_rows.clear();
    m_all.clear();
    m_byUid.clear();
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
    if (m_sortColumn == SortColumn(column) && m_sortDescending == descending)
        return;
    m_sortColumn = SortColumn(column);
    m_sortDescending = descending;
    resortVisible();
}

bool MessageListModel::lessThan(const Header &a, const Header &b) const
{
    int c;
    switch (m_sortColumn) {
    case SortColumn::From:
        c = QString::compare(a.fromKey, b.fromKey);
        break;
    case SortColumn::Subject:
        c = QString::compare(a.subjectKey, b.subjectKey);
        break;
    case SortColumn::Attachment:
        // Ties fall back to date so the groups stay chronological.
        c = int(a.attachKind != NoAttachment) - int(b.attachKind != NoAttachment);
        if (c == 0)
            c = a.dateSecs < b.dateSecs ? 1 : (b.dateSecs < a.dateSecs ? -1 : 0);
        break;
    default:
        c = a.dateSecs < b.dateSecs ? -1 : (b.dateSecs < a.dateSecs ? 1 : 0);
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

void MessageListModel::reindex()
{
    m_byUid.clear();
    m_byUid.reserve(m_all.size());
    for (int i = 0; i < m_all.size(); ++i)
        m_byUid.insert(m_all.at(i).uid, i);
}

void MessageListModel::rebuildVisible()
{
    QElapsedTimer timer;
    timer.start();
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(m_all.size());
    const bool filtered = hasFilter() || m_colorFilter != 0;
    for (int i = 0; i < m_all.size(); ++i) {
        if (!filtered || matchesFilter(m_all.at(i)))
            m_rows.append(i);
    }
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [this](int a, int b) { return lessThan(m_all.at(a), m_all.at(b)); });
    const qint64 sortMs = timer.elapsed();
    endResetModel();
    const qint64 totalMs = timer.elapsed();
    if (totalMs > kSlowMs) {
        qWarning() << "messagelist: SLOW rebuild" << m_all.size() << "rows," << m_rows.size()
                   << "visible; sort" << sortMs << "ms, reset" << (totalMs - sortMs) << "ms";
    }
}

void MessageListModel::resortVisible()
{
    if (m_rows.size() < 2)
        return;
    QElapsedTimer timer;
    timer.start();
    Q_EMIT layoutAboutToBeChanged({}, VerticalSortHint);

    const QList<int> before = m_rows;
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [this](int a, int b) { return lessThan(m_all.at(a), m_all.at(b)); });
    const qint64 sortMs = timer.elapsed();

    // Carry the view's persistent indices (selection, current row) over to
    // wherever their message ended up, so re-sorting does not reset them.
    QHash<int, int> rowOf;
    rowOf.reserve(m_rows.size());
    for (int row = 0; row < m_rows.size(); ++row)
        rowOf.insert(m_rows.at(row), row);
    const QModelIndexList from = persistentIndexList();
    QModelIndexList to;
    to.reserve(from.size());
    for (const QModelIndex &idx : from) {
        const int oldRow = idx.row();
        to.append(oldRow >= 0 && oldRow < before.size()
                      ? index(rowOf.value(before.at(oldRow), -1), idx.column())
                      : QModelIndex());
    }
    changePersistentIndexList(from, to);

    Q_EMIT layoutChanged({}, VerticalSortHint);
    const qint64 totalMs = timer.elapsed();
    if (totalMs > kSlowMs) {
        qWarning() << "messagelist: SLOW sort column" << int(m_sortColumn) << "over"
                   << m_rows.size() << "rows; sort" << sortMs << "ms, apply"
                   << (totalMs - sortMs) << "ms";
    }
}

qint64 MessageListModel::uidAt(int row) const
{
    return (row >= 0 && row < m_rows.size()) ? m_all.at(m_rows.at(row)).uid : -1;
}

int MessageListModel::rowForUid(qint64 uid) const
{
    if (uid < 0)
        return -1;
    const auto it = m_byUid.constFind(uid);
    return it == m_byUid.constEnd() ? -1 : visibleRowOf(it.value());
}

bool MessageListModel::seenAt(int row) const
{
    return row >= 0 && row < m_rows.size() && m_all.at(m_rows.at(row)).seen;
}

void MessageListModel::removeByUids(const QList<qint64> &uids)
{
    const QSet<qint64> gone(uids.begin(), uids.end());
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (gone.contains(m_all.at(m_rows.at(i)).uid)) {
            beginRemoveRows({}, i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }
    // Splicing m_all renumbers everything after the first hole, so the
    // surviving m_rows indices and the uid map are both rebuilt from scratch.
    const QList<qint64> visible = [this] {
        QList<qint64> out;
        out.reserve(m_rows.size());
        for (int idx : m_rows)
            out.append(m_all.at(idx).uid);
        return out;
    }();
    m_all.removeIf([&gone](const Header &h) { return gone.contains(h.uid); });
    reindex();
    m_rows.clear();
    m_rows.reserve(visible.size());
    for (qint64 uid : visible)
        m_rows.append(m_byUid.value(uid));
}

void MessageListModel::setAttachKind(qint64 uid, int kind)
{
    const auto it = m_byUid.constFind(uid);
    if (it == m_byUid.constEnd() || m_all.at(it.value()).attachKind == kind)
        return;
    m_all[it.value()].attachKind = kind;
    const int row = visibleRowOf(it.value());
    if (row >= 0) {
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {AttachmentRole, CalendarRole});
    }
}

int MessageListModel::colorLabelAt(int row) const
{
    return (row >= 0 && row < m_rows.size()) ? m_all.at(m_rows.at(row)).colorLabel : 0;
}

void MessageListModel::setColorLabel(qint64 uid, int color)
{
    const auto it = m_byUid.constFind(uid);
    if (it == m_byUid.constEnd())
        return;
    const int allIndex = it.value();
    m_all[allIndex].colorLabel = color;

    const int row = visibleRowOf(allIndex);
    // The change can move the row into or out of an active color filter —
    // insert/remove it instead of only repainting in place.
    const bool matches = matchesFilter(m_all.at(allIndex));
    if (row >= 0 && !matches) {
        beginRemoveRows({}, row, row);
        m_rows.removeAt(row);
        endRemoveRows();
    } else if (row < 0 && matches) {
        const auto cmp = [this](int a, int b) { return lessThan(m_all.at(a), m_all.at(b)); };
        const int at = int(std::upper_bound(m_rows.begin(), m_rows.end(), allIndex, cmp)
                           - m_rows.begin());
        beginInsertRows({}, at, at);
        m_rows.insert(at, allIndex);
        endInsertRows();
    } else if (row >= 0) {
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {ColorLabelRole});
    }
}

void MessageListModel::markSeen(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    m_all[m_rows.at(row)].seen = true;
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {SeenRole});
}
