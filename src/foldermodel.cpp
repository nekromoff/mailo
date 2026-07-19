#include "foldermodel.h"

#include <QSettings>

static QString collapsedSettingsKey(const QString &accountKey)
{
    // '/' would create nested QSettings groups — flatten it away.
    QString safe = accountKey;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("ui/collapsedFolders/") + safe;
}

int FolderModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible.size();
}

QVariant FolderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_visible.size())
        return {};
    const int allIndex = m_visible.at(index.row());
    const Folder &f = m_all.at(allIndex);
    switch (role) {
    case NameRole:
        return f.displayName;
    case MailBoxRole:
        return f.mailBox;
    case LevelRole:
        return f.level;
    case SelectableRole:
        return f.selectable;
    case HasChildrenRole:
        return hasChildren(allIndex);
    case ExpandedRole:
        return !m_collapsed.contains(f.mailBox);
    }
    return {};
}

QHash<int, QByteArray> FolderModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {MailBoxRole, "mailBox"},
        {LevelRole, "level"},
        {SelectableRole, "selectable"},
        {HasChildrenRole, "hasChildren"},
        {ExpandedRole, "expanded"},
    };
}

QStringList FolderModel::allMailBoxes() const
{
    QStringList out;
    out.reserve(m_all.size());
    for (const Folder &f : m_all)
        out.append(f.mailBox);
    return out;
}

QSet<QString> FolderModel::savedCollapsed(const QString &accountKey)
{
    if (accountKey.isEmpty())
        return {};
    QSettings s(QStringLiteral("mailo"), QStringLiteral("mailo"));
    const QStringList saved = s.value(collapsedSettingsKey(accountKey)).toStringList();
    return QSet<QString>(saved.begin(), saved.end());
}

void FolderModel::toggleSavedCollapsed(const QString &accountKey, const QString &mailBox)
{
    if (accountKey.isEmpty() || mailBox.isEmpty())
        return;
    QSettings s(QStringLiteral("mailo"), QStringLiteral("mailo"));
    const QString key = collapsedSettingsKey(accountKey);
    QStringList saved = s.value(key).toStringList();
    if (saved.removeAll(mailBox) == 0)
        saved.append(mailBox);
    s.setValue(key, saved);
}

void FolderModel::setAccountKey(const QString &key)
{
    m_accountKey = key;
    m_collapsed = savedCollapsed(key);
    beginResetModel();
    rebuildVisible();
    endResetModel();
}

void FolderModel::saveCollapsed() const
{
    if (m_accountKey.isEmpty())
        return;
    QSettings s(QStringLiteral("mailo"), QStringLiteral("mailo"));
    s.setValue(collapsedSettingsKey(m_accountKey), QStringList(m_collapsed.values()));
}

void FolderModel::setFolders(QList<Folder> folders)
{
    beginResetModel();
    m_all = std::move(folders);
    rebuildVisible();
    endResetModel();
}

bool FolderModel::hasChildren(int allIndex) const
{
    return allIndex + 1 < m_all.size()
        && m_all.at(allIndex + 1).level > m_all.at(allIndex).level;
}

void FolderModel::toggleExpanded(int row)
{
    if (row < 0 || row >= m_visible.size())
        return;
    const int allIndex = m_visible.at(row);
    if (!hasChildren(allIndex))
        return;
    const QString &mailBox = m_all.at(allIndex).mailBox;
    if (m_collapsed.contains(mailBox))
        m_collapsed.remove(mailBox);
    else
        m_collapsed.insert(mailBox);
    saveCollapsed();
    beginResetModel();
    rebuildVisible();
    endResetModel();
}

void FolderModel::rebuildVisible()
{
    m_visible.clear();
    int skipDeeperThan = -1; // hide rows below a collapsed ancestor
    for (int i = 0; i < m_all.size(); ++i) {
        const Folder &f = m_all.at(i);
        if (skipDeeperThan >= 0 && f.level > skipDeeperThan)
            continue;
        skipDeeperThan = -1;
        m_visible.append(i);
        if (m_collapsed.contains(f.mailBox))
            skipDeeperThan = f.level;
    }
}
