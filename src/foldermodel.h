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

private:
    void rebuildVisible();
    bool hasChildren(int allIndex) const;
    void saveCollapsed() const;

    QList<Folder> m_all;
    QList<int> m_visible;      ///< indices into m_all, collapsed subtrees hidden
    QSet<QString> m_collapsed; ///< mailBox paths whose children are hidden
    QString m_accountKey;
};
