// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <tuple>

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
    /// Marks a cached header as read, so the state survives a restart even
    /// before the next header sync confirms it from the server.
    void setSeen(const QString &folder, qint64 uid);
    /// Local-only color-scale mark (0 = none, 1..5), never synced to IMAP.
    void setColorLabel(const QString &folder, qint64 uid, int color);
    /// Every cached header of a folder carrying this color mark, newest first
    /// (indexed query — fast even on big folders).
    QList<MessageListModel::Header> headersByColor(const QString &folder, int color,
                                                   int limit = 1000);

    /// Cached UIDVALIDITY for a folder (0 = unknown).
    qint64 uidValidity(const QString &folder);
    void setUidValidity(const QString &folder, qint64 validity);
    /// Wipes every cached header/body/FTS row of a folder (UIDVALIDITY change).
    void clearFolder(const QString &folder);

    /// Re-keys the cached rows of \a oldFolder — and of its whole subtree —
    /// onto \a newFolder after a server-side RENAME, so a reparented folder
    /// keeps its offline mail instead of having to sync again. \a separator is
    /// the server's hierarchy delimiter; \a account scopes the folder keys.
    /// Rewrites body blobs, so it blocks: MailClient drives it on a worker
    /// connection, never on the GUI thread.
    static void renameFolderOn(QSqlDatabase &db, const QString &account,
                               const QString &oldFolder, const QString &newFolder,
                               QChar separator);

    /// The account-scoped storage key for a folder ("account\x1ffolder"), for
    /// the off-thread helpers below which have no access to the account state.
    QString scopedKey(const QString &folder) const { return scoped(folder); }

    /// Deletes at most \a limit cached messages (header + body + search rows)
    /// of \a scopedFolder, using \a db. Returns how many were removed; 0 means
    /// nothing is left. Deleting a body releases its blob pages, which is far
    /// too slow for the GUI thread at 100 KB a row — so this takes an explicit
    /// connection and is meant to be driven by purgeFolder() on a worker.
    static int purgeChunkOn(QSqlDatabase &db, const QString &scopedFolder, int limit);

    /// Runs purgeChunkOn() to completion on its own connection. Blocks, so
    /// call it from a worker thread. \a cancel aborts between chunks; \a
    /// progress is invoked with the running total after each chunk.
    static void purgeFolder(const QString &scopedFolder, const QAtomicInt &cancel,
                            const std::function<void(int)> &progress);

    /// Size of the cache file on disk, and how much of it is free pages that
    /// only a vacuum() can hand back to the filesystem.
    qint64 databaseBytes() const;
    qint64 reclaimableBytes();
    /// Rebuilds the database file, releasing free pages. Blocks for minutes on
    /// a large cache and locks out every other connection, so callers run it
    /// on a worker thread (see MailClient::reclaimDiskSpace). Opens its own
    /// connection, and so is the one store call safe to make off the GUI
    /// thread. Needs free disk space of roughly the current file size.
    static bool vacuum(QString *error = nullptr);

    /// Newest-first uids of cached headers that have no cached body yet —
    /// the work list for the idle-time body backfill.
    QList<qint64> uidsWithoutBody(const QString &folder, int limit = 10);
    /// How many cached headers still lack a cached body.
    int missingBodyCount(const QString &folder);

    /// Records that a body was deliberately not cached (over the size limit).
    /// Both queries above skip these, so the backfill stops asking for them.
    void skipBody(const QString &folder, qint64 uid, qint64 size);
    /// Makes skipped bodies eligible again after the limit is raised — those
    /// no bigger than \a maxSize (0 = no limit, clears the whole list).
    /// Returns how many became eligible.
    int unskipBodiesUpTo(qint64 maxSize);

    QByteArray cachedBody(const QString &folder, qint64 uid);
    /// Drops just the cached body (and its part references) of one message,
    /// keeping the header. Used when a stub turns out to reference a payload
    /// that is no longer on disk, so the next open re-fetches it cleanly.
    void removeBodyOnly(const QString &folder, qint64 uid);
    void storeBody(const QString &folder, qint64 uid, const QByteArray &raw,
                   const QString &indexText);

    /// One attachment payload lifted out of a message into the file store.
    struct PartRef {
        QString partId;   ///< MIME path within the message, e.g. "2.1"
        QString hash;     ///< content address in AttachmentStore
        QString filename;
        QString mime;
        qint64 size = 0;  ///< decoded size
        qint64 stored = 0;///< bytes on disk after compression
        int codec = 0;    ///< 0 = raw, 1 = zstd
    };
    /// Records the parts lifted out of one message and takes a reference on
    /// each payload. Call inside the same transaction as its stub write.
    void storeParts(const QString &folder, qint64 uid, const QList<PartRef> &parts);
    /// Same, on an explicit connection and an already-scoped folder key.
    static void storePartsOn(QSqlDatabase &db, const QString &scopedFolder, qint64 uid,
                             const QList<PartRef> &parts);
    /// The parts of a message, for putting the payloads back when it is read.
    QList<PartRef> partsFor(const QString &folder, qint64 uid);
    /// Drops the part rows of the given messages and deletes any payload whose
    /// last referrer just went away. Returns bytes freed on disk.
    qint64 releaseParts(const QString &scopedFolder, const QList<qint64> &uids);
    /// Same, on an explicit connection, for the workers that do not own m_db.
    static qint64 releasePartsOn(QSqlDatabase &db, const QString &scopedFolder,
                                 const QList<qint64> &uids);
    /// Total size of the attachment file store, and how much of it is unused.
    qint64 attachmentBytes();
    /// True until every pre-existing body has had its attachments moved into
    /// the file store. Survives restarts, so an interrupted run resumes.
    bool attachmentMigrationPending();
    /// Splits one chunk of already-cached bodies. \a splitFn receives a raw
    /// message and returns its stub, filling the part list — it is a callback
    /// so the store stays free of MIME knowledge. Advances \a cursor and
    /// returns how many rows were examined; 0 means the migration is finished.
    /// Runs on \a db, i.e. on a worker thread.
    static int migrateAttachmentsChunk(
        QSqlDatabase &db, qint64 &cursor, int limit, qint64 &bytesSaved,
        const std::function<QByteArray(const QByteArray &, QList<PartRef> *)> &splitFn);
    /// Marks the migration complete (worker connection).
    static void finishAttachmentMigration(QSqlDatabase &db);

    /// Deletes payload files with no referring row, left behind by a run that
    /// was interrupted between writing a file and committing its part row.
    /// Returns how many were removed.
    int sweepOrphanAttachments();

    /// One cached body waiting to be written, already account-scoped.
    struct BodyWrite {
        QString scopedFolder;
        qint64 uid = 0;
        QByteArray raw;       ///< the stub: message with big payloads removed
        QString indexText;
        QList<PartRef> parts; ///< payloads lifted into the file store
    };
    /// Writes a batch of bodies in a single transaction on \a db. Storing a
    /// ~100 KB blob plus its FTS rows costs tens of ms, so the GUI thread does
    /// not do this — MailClient drains its queue on a writer thread.
    static void writeBodiesOn(QSqlDatabase &db, const QList<BodyWrite> &batch);
    /// Opens an additional connection to the cache for a worker thread.
    /// \a name must be unique per thread. Returns an invalid db on failure.
    static QSqlDatabase openWorkerConnection(const QString &name);

    /// Local keyword search inside one folder; matches partial words too
    /// (FTS5 prefix query plus a substring scan over subject/sender).
    /// Blocking, and on a large folder the substring pass is not fast — callers
    /// on the GUI thread should use searchOn() on a worker instead.
    QList<MessageListModel::Header> search(const QString &folder, const QString &keyword);

    /// Receives search hits in batches as they are found; returning false
    /// abandons the search (a newer query, a folder switch, shutdown).
    using SearchSink = std::function<bool(const QList<MessageListModel::Header> &)>;

    /// search(), on a worker thread's connection, delivering as it goes.
    /// \a scopedFolder is what scopedKey() returns for the folder.
    /// \a headersOnly limits the full-text pass to sender and subject.
    static void searchOn(QSqlDatabase &db, const QString &scopedFolder, const QString &keyword,
                         bool ftsAvailable, const SearchSink &deliver, bool headersOnly = false);

    /// Whether the FTS index exists — searchOn() takes this as a parameter
    /// because it cannot ask the instance from a worker thread.
    bool ftsAvailable() const { return m_ftsAvailable; }

    /// True when the search index was built before diacritic folding, so
    /// "ave" does not find "ávé". An fts5 tokenizer cannot be changed in
    /// place; the index has to be copied into a new table. Noted at open(),
    /// carried out by MailClient on a worker.
    bool ftsNeedsRebuild() const { return m_ftsRebuildNeeded; }
    /// Creates the folded index if it is not there yet.
    static bool beginFtsRebuild(QSqlDatabase &db);
    /// Where a previous run stopped (0 = nothing copied yet).
    static qint64 ftsRebuildCursor(QSqlDatabase &db);
    /// Copies up to \a limit rows past \a cursor, advancing it. Returns the
    /// number copied, 0 when the old index is exhausted, -1 on failure.
    static int copyFtsChunk(QSqlDatabase &db, qint64 *cursor, int limit);
    /// Swaps the folded index in for the old one. Returns false if the swap
    /// failed, in which case the old index is untouched and still usable.
    static bool finishFtsRebuild(QSqlDatabase &db);
    /// Queues the given bodies for background re-indexing (fts_pending).
    static void queueForReindex(QSqlDatabase &db, const QList<BodyWrite> &batch);
    /// Rough denominator for rebuild progress.
    static qint64 indexedMessageCount(QSqlDatabase &db);

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
    /// Same, for a whole batch in one transaction — one fsync instead of N.
    /// Entries are (scopedFolder, uid, indexText).
    void finishBodyIndexBatch(const QList<std::tuple<QString, qint64, QString>> &entries);

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
    /// Message-ID from a raw header block, angle brackets stripped. Empty when
    /// the message has none (which is legal, if rare).
    static QString messageIdFromHead(const QByteArray &head);

    /// Fills in msgid for up to \a limit cached rows that do not have one yet,
    /// reading only each message's head rather than its payload. Returns how
    /// many rows it touched; 0 means there is nothing left to do. Deliberately
    /// chunked and resumable — the bodies table is multiple gigabytes and this
    /// must never become one long scan.
    int backfillMessageIds(int limit);

    /// Every cached copy of \a msgid, as (folder, uid). More than one is normal:
    /// the same message commonly exists in a folder and in All Mail.
    QList<QPair<QString, qint64>> locateByMessageId(const QString &msgid);

private:
    /// Folder key as stored in messages/bodies/fts: "account\x1ffolder".
    QString scoped(const QString &folder) const;

    QSqlDatabase m_db;
    QString m_accountKey;
    bool m_ftsAvailable = false;
    bool m_ftsRebuildNeeded = false; ///< index predates diacritic folding
};
