// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>

/// Flat list of IMAP mailboxes (hierarchy shown via indentation level).
/// Folders with children can be collapsed, hiding their subtree.
class FolderModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        MailBoxRole,
        LevelRole,
        SelectableRole,
        HasChildrenRole,
        ExpandedRole
    };

    struct Folder {
        QString displayName;
        QString mailBox; // full IMAP path, e.g. "INBOX/Archive"
        int level = 0;
        bool selectable = true;
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setFolders(QList<Folder> folders);

    /// Identifies the account ("user@host") so collapsed state persists per
    /// account across restarts; loads that account's saved state.
    void setAccountKey(const QString &key);

    /// The collapsed mailbox paths saved for an account, without needing a
    /// model instance (used for rendering non-current accounts' cached trees).
    static QSet<QString> savedCollapsed(const QString &accountKey);
    /// Flips one mailbox in an account's persisted collapsed set — the
    /// collapse toggle for cached (non-current) folder trees.
    static void toggleSavedCollapsed(const QString &accountKey, const QString &mailBox);

    /// Every known mailbox path, including ones hidden by collapsed parents.
    QStringList allMailBoxes() const;

    Q_INVOKABLE void toggleExpanded(int row);
    /// Reveals a row's children if they are hidden. A no-op when the row has
    /// none or is already expanded — so opening a folder can call it
    /// unconditionally without forcing a model reset on every click.
    Q_INVOKABLE void expandRow(int row);

    /// Visible row of \a mailBox, or -1 when it is unknown or hidden inside a
    /// collapsed parent. The view cannot answer this itself: ListView only
    /// instantiates delegates near the viewport, so itemAtIndex() returns null
    /// for rows that are merely scrolled out of sight.
    Q_INVOKABLE int rowForMailBox(const QString &mailBox) const;
    /// Full IMAP path at a visible row (empty when out of range).
    Q_INVOKABLE QString mailBoxAt(int row) const;
    /// Whether the folder at a visible row can be opened (not \Noselect).
    Q_INVOKABLE bool selectableAt(int row) const;

private:
    void rebuildVisible();
    bool hasChildren(int allIndex) const;
    void saveCollapsed() const;

    QList<Folder> m_all;
    QList<int> m_visible;      ///< indices into m_all, collapsed subtrees hidden
    QSet<QString> m_collapsed; ///< mailBox paths whose children are hidden
    QString m_accountKey;
};
