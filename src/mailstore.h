#pragma once

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "messagelistmodel.h"

/**
 * SQLite cache: message headers per folder, raw message bodies, and an FTS5
 * full-text index (subject/sender/body). Folders open instantly from cache
 * while the network refresh runs, previously read messages open offline,
 * and keyword search hits the local index before falling back further.
 *
 * All message/body/search rows are keyed per account (setAccountKey) so
 * several accounts never mix their identically-named folders (INBOX…).
 * Folder lists are stored per account explicitly so the folder tree of an
 * inactive account can still be shown in the sidebar.
 *
 * v1 ignores UIDVALIDITY — if a server resets UIDs the cache heals itself
 * on the next header refresh (INSERT OR REPLACE), stale bodies just expire.
 */
class MailStore
{
public:
    bool open();

    /// Scopes all folder-keyed operations below to this account ("user@host").
    void setAccountKey(const QString &key);
    /// One-time migration: claims cache rows written before accounts were
    /// separated (unscoped folder keys, global folder list) for \a account.
    /// No-op once the legacy rows are gone.
    void adoptLegacyCache(const QString &account);

    QStringList cachedFolders(const QString &account);
    void storeFolders(const QString &account, const QStringList &folders);

    QList<MessageListModel::Header> cachedHeaders(const QString &folder, int limit = 1000);
    /// Next page of cached headers strictly older than the (date, uid) anchor
    /// — keyset pagination for endless scrolling through the disk cache.
    QList<MessageListModel::Header> cachedHeadersBefore(const QString &folder, qint64 dateSecs,
                                                        qint64 uid, int limit = 500);
    /// Number of cached headers of a folder (no row limit).
    int cachedHeaderCount(const QString &folder);
    /// Highest cached uid of a folder (0 when nothing is cached).
    qint64 maxCachedUid(const QString &folder);
    void storeHeaders(const QString &folder, const QList<MessageListModel::Header> &headers);
    void removeMessages(const QString &folder, const QList<qint64> &uids);
    /// Refined attachment kind (MessageListModel::Header::attachKind) learned
    /// from the full body — e.g. "single .ics calendar invite".
    void setAttachKind(const QString &folder, qint64 uid, int kind);

    /// Cached UIDVALIDITY for a folder (0 = unknown).
    qint64 uidValidity(const QString &folder);
    void setUidValidity(const QString &folder, qint64 validity);
    /// Wipes every cached header/body/FTS row of a folder (UIDVALIDITY change).
    void clearFolder(const QString &folder);

    /// Newest-first uids of cached headers that have no cached body yet —
    /// the work list for the idle-time body backfill.
    QList<qint64> uidsWithoutBody(const QString &folder, int limit = 10);
    /// How many cached headers still lack a cached body.
    int missingBodyCount(const QString &folder);

    QByteArray cachedBody(const QString &folder, qint64 uid);
    void storeBody(const QString &folder, qint64 uid, const QByteArray &raw,
                   const QString &indexText);

    /// Local keyword search inside one folder; matches partial words too
    /// (FTS5 prefix query plus a substring scan over subject/sender).
    QList<MessageListModel::Header> search(const QString &folder, const QString &keyword);

    /// A cached body whose text still has to be (re)indexed for search.
    struct PendingBody {
        QString scopedFolder; ///< raw folder key as stored ("account\x1ffolder")
        qint64 uid = 0;
        QByteArray raw;
    };
    /// Next batch of bodies awaiting search indexing (fts rebuild work list).
    QList<PendingBody> pendingBodyIndex(int limit);
    /// Writes the extracted body text into the search index and removes the
    /// entry from the work list. \a scopedFolder as given by pendingBodyIndex.
    void finishBodyIndex(const QString &scopedFolder, qint64 uid, const QString &indexText);

    /// Per-sender "load remote content" preference (sender = addr-spec, lowercase).
    bool remoteContentAllowedFor(const QString &sender);
    void setRemoteContentAllowedFor(const QString &sender, bool allowed);

    /// Remembers an address mail was sent to (compose autocompletion).
    /// Repeated adds bump a use counter that ranks the suggestions.
    void addRecipient(const QString &address, const QString &name = {});
    /// Known recipient addresses matching \a prefix (substring of the address
    /// or display name), best-ranked first.
    QStringList recipientCompletions(const QString &prefix, int limit = 8);
    /// One-time sweep of the cached raw bodies of \a sentFolder, seeding the
    /// recipient list with every To/Cc address found there. Remembers per
    /// account that it ran, so later calls are no-ops.
    void harvestSentRecipients(const QString &sentFolder);

    /// Attachment heuristic on a raw RFC-2822 head: top-level multipart/mixed.
    /// Works on the raw bytes because KMime downgrades multipart/* to
    /// text/plain when parsing a header-only (body-less) message shell.
    static bool headIndicatesAttachment(const QByteArray &head);

private:
    /// Folder key as stored in messages/bodies/fts: "account\x1ffolder".
    QString scoped(const QString &folder) const;

    QSqlDatabase m_db;
    QString m_accountKey;
    bool m_ftsAvailable = false;
};
