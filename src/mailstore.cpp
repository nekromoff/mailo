// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "mailstore.h"

#include "attachmentstore.h"

#include <QDateTime>
#include <QHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>

/// The store runs on the GUI thread: any call here directly delays input
/// handling and rendering. The UI budget is 20 ms — make violations loud.
namespace
{
struct SlowGuard {
    explicit SlowGuard(const char *operation)
        : op(operation)
    {
        timer.start();
    }
    ~SlowGuard()
    {
        const qint64 ms = timer.elapsed();
        if (ms > 20)
            qWarning() << "mailstore: SLOW" << op << ms << "ms on the GUI thread";
    }
    QElapsedTimer timer;
    const char *op;
};

/// One-time migrations record themselves in meta_flags so they cost a single
/// indexed lookup on every later start instead of a full-table pass. Both
/// helpers assume meta_flags exists (open() creates it first thing).
bool migrationDone(const QSqlDatabase &db, const QString &flag)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM meta_flags WHERE flag = ?"));
    q.addBindValue(flag);
    return q.exec() && q.next();
}

void markMigrationDone(const QSqlDatabase &db, const QString &flag)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO meta_flags (flag) VALUES (?)"));
    q.addBindValue(flag);
    q.exec();
}
}

bool MailStore::open()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    // Cached mail is private data — owner-only on the whole directory so the
    // -wal/-shm side files are covered too.
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("mailstore"));
    m_db.setDatabaseName(dir + QStringLiteral("/mailo.db"));
    if (!m_db.open()) {
        qWarning() << "mailstore: cannot open database:" << m_db.lastError().text();
        return false;
    }
    QFile::setPermissions(dir + QStringLiteral("/mailo.db"),
                          QFile::ReadOwner | QFile::WriteOwner);

    SlowGuard guard("open");
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    // The purge and vacuum workers write on their own connections. Without a
    // busy timeout this connection would fail its writes outright the moment
    // one of them held the lock, instead of waiting the few ms it takes.
    q.exec(QStringLiteral("PRAGMA busy_timeout=15000"));

    // Gates every one-time migration below, so create it before the first one.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_flags (flag TEXT PRIMARY KEY)"));
    // Resumable progress for migrations too long to finish in one run.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_values ("
                          " key TEXT PRIMARY KEY, value TEXT)"));
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages ("
            " folder TEXT NOT NULL, uid INTEGER NOT NULL,"
            " subject TEXT, sender TEXT, date INTEGER, seen INTEGER DEFAULT 0,"
            " PRIMARY KEY(folder, uid))"))) {
        qWarning() << "mailstore: schema failed:" << q.lastError().text();
        return false;
    }
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS bodies ("
        " folder TEXT NOT NULL, uid INTEGER NOT NULL, raw BLOB,"
        " PRIMARY KEY(folder, uid))"));

    // Sweep ghost rows cached by earlier versions: entries without a uid or
    // with no content at all ("(no subject), 1970") can never be opened.
    // Neither predicate is indexable, so this is a full pass over messages and
    // bodies — once, not on every start, since no current code writes them.
    if (!migrationDone(m_db, QStringLiteral("ghost_sweep1"))) {
        q.exec(QStringLiteral("DELETE FROM messages WHERE uid <= 0"
                              " OR (IFNULL(subject,'') = '' AND IFNULL(sender,'') = ''"
                              "     AND IFNULL(date,0) <= 0)"));
        q.exec(QStringLiteral("DELETE FROM bodies WHERE uid <= 0"));
        markMigrationDone(m_db, QStringLiteral("ghost_sweep1"));
    }

    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS folders ("
                          " mailbox TEXT PRIMARY KEY, sortkey INTEGER)"));

    // Per-account folder lists (the legacy global "folders" table is adopted
    // into this one on first run, see adoptLegacyCache()).
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS account_folders ("
                          " account TEXT NOT NULL, mailbox TEXT NOT NULL,"
                          " sortkey INTEGER, uidvalidity INTEGER DEFAULT 0,"
                          " PRIMARY KEY(account, mailbox))"));

    // Attachment payloads live in files keyed by content hash (see
    // attachmentstore.h); these two tables are the index over that store.
    // `refs` is what makes eviction possible at all — the old
    // (folder, uid) -> BLOB layout had nowhere to record that the same
    // payload was reachable from several messages.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS attachments ("
                          " hash TEXT PRIMARY KEY, size INTEGER NOT NULL,"
                          " stored INTEGER NOT NULL, codec INTEGER NOT NULL DEFAULT 0,"
                          " refs INTEGER NOT NULL DEFAULT 0, last_used INTEGER DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS message_parts ("
                          " folder TEXT NOT NULL, uid INTEGER NOT NULL, part_id TEXT NOT NULL,"
                          " hash TEXT NOT NULL, filename TEXT, mime TEXT, encoding TEXT,"
                          " PRIMARY KEY(folder, uid, part_id))"));
    // Dropping a folder has to find its parts by (folder, uid); releasing a
    // payload has to count the remaining referrers by hash.
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_message_parts_hash"
                          " ON message_parts(hash)"));

    // The first attachment migration mis-reported the codec of deduplicated
    // payloads and skipped those messages, having already moved its cursor
    // past them. Rewind once so they get another pass; messages already
    // migrated simply parse, find nothing large inline, and cost one read.
    if (!migrationDone(m_db, QStringLiteral("attach_migrate_reset1"))) {
        q.exec(QStringLiteral(
            "DELETE FROM meta_values WHERE key = 'attach_migrate_cursor'"));
        q.exec(QStringLiteral("DELETE FROM meta_flags WHERE flag = 'attach_migrate1'"));
        markMigrationDone(m_db, QStringLiteral("attach_migrate_reset1"));
    }

    // Bodies deliberately not cached because they exceed the size limit.
    // Without this the backfill would ask for them again on every single pass:
    // uidsWithoutBody() cannot tell "never fetched" from "refused to store".
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS body_skipped ("
                          " folder TEXT NOT NULL, uid INTEGER NOT NULL,"
                          " size INTEGER DEFAULT 0,"
                          " PRIMARY KEY(folder, uid))"));

    // Senders the user chose to always load remote content for.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS remote_senders ("
                          " sender TEXT PRIMARY KEY)"));

    // Addresses mail was sent to, for compose autocompletion.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS recipients ("
                          " account TEXT NOT NULL, address TEXT NOT NULL,"
                          " name TEXT DEFAULT '', last_used INTEGER DEFAULT 0,"
                          " use_count INTEGER DEFAULT 0,"
                          " PRIMARY KEY(account, address))"));

    // Schema upgrades for pre-existing databases; fail silently if present.
    q.exec(QStringLiteral("ALTER TABLE folders ADD COLUMN uidvalidity INTEGER DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN suspicious INTEGER DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN auth TEXT DEFAULT ''"));
    q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN attach INTEGER DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN color INTEGER DEFAULT 0"));
    // (folder, color) alone could find the rows but not order them, so the
    // colour filter still sorted the whole folder. Carrying the sort keys in
    // the index makes it a seek plus a LIMIT.
    if (!migrationDone(m_db, QStringLiteral("color_index2"))) {
        q.exec(QStringLiteral("DROP INDEX IF EXISTS idx_messages_color"));
        markMigrationDone(m_db, QStringLiteral("color_index2"));
    }
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_color"
                          " ON messages(folder, color, date DESC, uid DESC)"));
    // Every list query is "newest first within a folder". Without this the
    // only usable index is the (folder, uid) primary key, which yields uid
    // order — so SQLite read the whole folder into a temp B-tree and sorted it
    // before applying LIMIT. On a 200k-message folder that is a full sort to
    // show 1000 rows, on the GUI thread, on every open, scroll and search.
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_date"
                          " ON messages(folder, date DESC, uid DESC)"));

    m_ftsAvailable = q.exec(QStringLiteral(
        "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5("
        " subject, sender, body, folder UNINDEXED, uid UNINDEXED)"));
    if (!m_ftsAvailable)
        qWarning() << "mailstore: FTS5 unavailable:" << q.lastError().text();

    // One-time rebuild keying every fts row by its messages.rowid. folder/uid
    // are UNINDEXED in fts5, so per-message maintenance filtered on them was
    // a full scan of the whole index — O(index) CPU on the GUI thread for
    // EVERY header stored and body cached. rowid lookups are O(1).
    if (m_ftsAvailable
        && (!q.exec(QStringLiteral("SELECT 1 FROM meta_flags WHERE flag = 'fts_rowid'"))
            || !q.next())) {
        q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_flags (flag TEXT PRIMARY KEY)"));
        m_db.transaction();
        QSqlQuery mig(m_db);
        mig.exec(QStringLiteral(
            "CREATE VIRTUAL TABLE fts_new USING fts5("
            " subject, sender, body, folder UNINDEXED, uid UNINDEXED)"));
        mig.exec(QStringLiteral(
            "INSERT INTO fts_new (rowid, subject, sender, body, folder, uid)"
            " SELECT m.rowid, f.subject, f.sender, f.body, f.folder, f.uid"
            " FROM fts f JOIN messages m ON m.folder = f.folder AND m.uid = f.uid"));
        mig.exec(QStringLiteral("DROP TABLE fts"));
        mig.exec(QStringLiteral("ALTER TABLE fts_new RENAME TO fts"));
        q.exec(QStringLiteral("INSERT OR IGNORE INTO meta_flags (flag) VALUES ('fts_rowid')"));
        m_db.commit();
    }

    // Self-healing index rebuild: (re)creates the header rows for every
    // message missing from fts in one statement, and queues every cached
    // body for background text re-indexing (fts_pending). Runs once; if any
    // step fails (e.g. the DB is locked by another instance) nothing is
    // committed and it retries on the next start.
    if (m_ftsAvailable
        && (!q.exec(QStringLiteral("SELECT 1 FROM meta_flags WHERE flag = 'fts_rebuild1'"))
            || !q.next())) {
        q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_flags (flag TEXT PRIMARY KEY)"));
        m_db.transaction();
        QSqlQuery mig(m_db);
        bool ok = mig.exec(QStringLiteral(
            "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
            " SELECT m.rowid, IFNULL(m.subject, ''), IFNULL(m.sender, ''), '', m.folder, m.uid"
            " FROM messages m"
            " WHERE NOT EXISTS (SELECT 1 FROM fts WHERE rowid = m.rowid)"));
        ok = mig.exec(QStringLiteral(
                 "CREATE TABLE IF NOT EXISTS fts_pending ("
                 " folder TEXT NOT NULL, uid INTEGER NOT NULL,"
                 " PRIMARY KEY(folder, uid))"))
            && ok;
        ok = mig.exec(QStringLiteral(
                 "INSERT OR IGNORE INTO fts_pending (folder, uid)"
                 " SELECT folder, uid FROM bodies"))
            && ok;
        if (ok) {
            q.exec(QStringLiteral(
                "INSERT OR IGNORE INTO meta_flags (flag) VALUES ('fts_rebuild1')"));
            m_db.commit();
        } else {
            qWarning() << "mailstore: fts rebuild failed, retrying next start:"
                       << mig.lastError().text();
            m_db.rollback();
        }
    }

    // One-time backfill: rows cached before the attach column existed get
    // their flag recomputed from the raw bodies we already have on disk.
    if (!q.exec(QStringLiteral("SELECT 1 FROM meta_flags WHERE flag = 'attach_backfill'"))
        || !q.next()) {
        q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_flags (flag TEXT PRIMARY KEY)"));
        QSqlQuery bodies(m_db);
        QSqlQuery upd(m_db);
        upd.prepare(QStringLiteral(
            "UPDATE messages SET attach = 1 WHERE folder = ? AND uid = ?"));
        m_db.transaction();
        if (bodies.exec(QStringLiteral("SELECT folder, uid, raw FROM bodies"))) {
            while (bodies.next()) {
                const QByteArray raw = bodies.value(2).toByteArray();
                const int headEnd = raw.indexOf("\r\n\r\n") >= 0
                    ? raw.indexOf("\r\n\r\n") : raw.indexOf("\n\n");
                if (headIndicatesAttachment(headEnd > 0 ? raw.left(headEnd) : raw)) {
                    upd.addBindValue(bodies.value(0));
                    upd.addBindValue(bodies.value(1));
                    upd.exec();
                }
            }
        }
        q.exec(QStringLiteral("INSERT OR IGNORE INTO meta_flags (flag) VALUES ('attach_backfill')"));
        m_db.commit();
    }
    return true;
}

void MailStore::setAccountKey(const QString &key)
{
    m_accountKey = key;
}

QString MailStore::scoped(const QString &folder) const
{
    if (m_accountKey.isEmpty())
        return folder;
    return m_accountKey + QChar(0x1f) + folder;
}

void MailStore::adoptLegacyCache(const QString &account)
{
    if (!m_db.isOpen() || account.isEmpty())
        return;
    // Strictly a first-run upgrade step. The instr() predicates below cannot
    // use an index, so re-running it on an already-scoped cache scanned every
    // messages/bodies/fts row for nothing — the single largest cost in
    // startup. Once claimed, no unscoped row can appear again.
    if (migrationDone(m_db, QStringLiteral("legacy_adopt1")))
        return;
    SlowGuard guard("adoptLegacyCache");
    m_db.transaction();
    QSqlQuery q(m_db);

    // Global folder list → this account's per-account list.
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO account_folders (account, mailbox, sortkey, uidvalidity)"
        " SELECT ?, mailbox, sortkey, uidvalidity FROM folders"));
    q.addBindValue(account);
    q.exec();
    q.exec(QStringLiteral("DELETE FROM folders"));

    // Unscoped message/body/index rows get this account's folder-key prefix.
    // instr() guards make this idempotent — prefixed rows are left alone.
    const QString prefix = account + QChar(0x1f);
    for (const char *table : {"messages", "bodies", "fts"}) {
        if (!m_ftsAvailable && qstrcmp(table, "fts") == 0)
            continue;
        QSqlQuery upd(m_db);
        upd.prepare(QStringLiteral(
                        "UPDATE %1 SET folder = ? || folder WHERE instr(folder, char(31)) = 0")
                        .arg(QLatin1String(table)));
        upd.addBindValue(prefix);
        upd.exec();
    }
    if (m_db.commit())
        markMigrationDone(m_db, QStringLiteral("legacy_adopt1"));
}

QStringList MailStore::cachedFolders(const QString &account)
{
    QStringList out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT mailbox FROM account_folders WHERE account = ? ORDER BY sortkey"));
    q.addBindValue(account);
    if (q.exec()) {
        while (q.next())
            out.append(q.value(0).toString());
    }
    return out;
}

void MailStore::storeFolders(const QString &account, const QStringList &folders)
{
    if (!m_db.isOpen())
        return;
    m_db.transaction();
    QSqlQuery q(m_db);
    // Preserve each folder's cached UIDVALIDITY across the rewrite.
    QHash<QString, qint64> validity;
    q.prepare(QStringLiteral(
        "SELECT mailbox, uidvalidity FROM account_folders WHERE account = ?"));
    q.addBindValue(account);
    if (q.exec()) {
        while (q.next())
            validity.insert(q.value(0).toString(), q.value(1).toLongLong());
    }
    q.prepare(QStringLiteral("DELETE FROM account_folders WHERE account = ?"));
    q.addBindValue(account);
    q.exec();
    q.prepare(QStringLiteral("INSERT INTO account_folders"
                             " (account, mailbox, sortkey, uidvalidity) VALUES (?, ?, ?, ?)"));
    for (int i = 0; i < folders.size(); ++i) {
        q.addBindValue(account);
        q.addBindValue(folders.at(i));
        q.addBindValue(i);
        q.addBindValue(validity.value(folders.at(i), 0));
        q.exec();
    }
    m_db.commit();
}

static QList<MessageListModel::Header> readHeaderRows(QSqlQuery &q)
{
    QList<MessageListModel::Header> out;
    if (!q.exec())
        return out;
    while (q.next()) {
        MessageListModel::Header h;
        h.uid = q.value(0).toLongLong();
        h.subject = q.value(1).toString();
        h.from = q.value(2).toString();
        h.date = QDateTime::fromSecsSinceEpoch(q.value(3).toLongLong());
        h.seen = q.value(4).toBool();
        h.suspicious = q.value(5).toBool();
        h.authInfo = q.value(6).toString();
        h.attachKind = q.value(7).toInt();
        h.colorLabel = q.value(8).toInt();
        out.append(h);
    }
    return out;
}

QList<MessageListModel::Header> MailStore::cachedHeaders(const QString &folder, int limit)
{
    if (!m_db.isOpen())
        return {};
    SlowGuard guard("cachedHeaders");
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid, subject, sender, date, seen, suspicious, auth, attach,"
                             " color FROM messages WHERE folder = ?"
                             " ORDER BY date DESC, uid DESC LIMIT ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(limit);
    return readHeaderRows(q);
}

QList<MessageListModel::Header> MailStore::cachedHeadersBefore(const QString &folder,
                                                               qint64 dateSecs, qint64 uid,
                                                               int limit)
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid, subject, sender, date, seen, suspicious, auth, attach,"
                             " color FROM messages WHERE folder = ?"
                             " AND (date < ? OR (date = ? AND uid < ?))"
                             " ORDER BY date DESC, uid DESC LIMIT ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(dateSecs);
    q.addBindValue(dateSecs);
    q.addBindValue(uid);
    q.addBindValue(limit);
    return readHeaderRows(q);
}

QList<MessageListModel::Header> MailStore::headersByColor(const QString &folder, int color,
                                                          int limit)
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid, subject, sender, date, seen, suspicious, auth, attach,"
                             " color FROM messages WHERE folder = ? AND color = ?"
                             " ORDER BY date DESC, uid DESC LIMIT ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(color);
    q.addBindValue(limit);
    return readHeaderRows(q);
}

int MailStore::cachedHeaderCount(const QString &folder)
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages WHERE folder = ?"));
    q.addBindValue(scoped(folder));
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
}

qint64 MailStore::maxCachedUid(const QString &folder)
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT MAX(uid) FROM messages WHERE folder = ?"));
    q.addBindValue(scoped(folder));
    return (q.exec() && q.next()) ? q.value(0).toLongLong() : 0;
}

void MailStore::storeHeaders(const QString &folder,
                             const QList<MessageListModel::Header> &headers)
{
    if (!m_db.isOpen() || headers.isEmpty())
        return;
    SlowGuard guard("storeHeaders");
    m_db.transaction();
    const QString key = scoped(folder);
    QSqlQuery q(m_db);
    // Header refreshes only know "has attachment or not" (attach 0/1); a
    // refined kind (2 = calendar invite) learned from the full body survives.
    q.prepare(QStringLiteral(
        "INSERT INTO messages"
        " (folder, uid, subject, sender, date, seen, suspicious, auth, attach)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(folder, uid) DO UPDATE SET"
        " subject = excluded.subject, sender = excluded.sender, date = excluded.date,"
        // A locally-read message stays read even if the server still reports
        // \Unseen — e.g. it was read offline and the STORE never went out.
        " seen = MAX(messages.seen, excluded.seen),"
        " suspicious = excluded.suspicious, auth = excluded.auth,"
        " attach = CASE WHEN messages.attach > 1 AND excluded.attach = 1"
        " THEN messages.attach ELSE excluded.attach END"));
    QSqlQuery ins(m_db);
    if (m_ftsAvailable) {
        // Keyed by messages.rowid — an O(1) lookup. Never filter fts by its
        // UNINDEXED folder/uid columns here: that is a full-index scan.
        ins.prepare(QStringLiteral(
            "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
            " SELECT m.rowid, ?, ?, '', ?, ? FROM messages m"
            " WHERE m.folder = ? AND m.uid = ?"
            " AND NOT EXISTS (SELECT 1 FROM fts WHERE rowid = m.rowid)"));
    }
    for (const auto &h : headers) {
        q.addBindValue(key);
        q.addBindValue(h.uid);
        q.addBindValue(h.subject);
        q.addBindValue(h.from);
        q.addBindValue(h.date.toSecsSinceEpoch());
        q.addBindValue(h.seen ? 1 : 0);
        q.addBindValue(h.suspicious ? 1 : 0);
        q.addBindValue(h.authInfo);
        q.addBindValue(h.attachKind);
        q.exec();
        if (m_ftsAvailable) {
            ins.addBindValue(h.subject);
            ins.addBindValue(h.from);
            ins.addBindValue(key);
            ins.addBindValue(h.uid);
            ins.addBindValue(key);
            ins.addBindValue(h.uid);
            ins.exec();
        }
    }
    m_db.commit();
}

void MailStore::setAttachKind(const QString &folder, qint64 uid, int kind)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET attach = ? WHERE folder = ? AND uid = ?"));
    q.addBindValue(kind);
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    q.exec();
}

void MailStore::setColorLabel(const QString &folder, qint64 uid, int color)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET color = ? WHERE folder = ? AND uid = ?"));
    q.addBindValue(color);
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    q.exec();
}

void MailStore::setSeen(const QString &folder, qint64 uid)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET seen = 1 WHERE folder = ? AND uid = ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    q.exec();
}

QList<qint64> MailStore::uidsWithoutBody(const QString &folder, int limit)
{
    QList<qint64> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT m.uid FROM messages m"
        " LEFT JOIN bodies b ON b.folder = m.folder AND b.uid = m.uid"
        " LEFT JOIN body_skipped s ON s.folder = m.folder AND s.uid = m.uid"
        " WHERE m.folder = ? AND b.uid IS NULL AND s.uid IS NULL"
        " ORDER BY m.date DESC, m.uid DESC LIMIT ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next())
            out.append(q.value(0).toLongLong());
    }
    return out;
}

int MailStore::missingBodyCount(const QString &folder)
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM messages m"
        " LEFT JOIN bodies b ON b.folder = m.folder AND b.uid = m.uid"
        " LEFT JOIN body_skipped s ON s.folder = m.folder AND s.uid = m.uid"
        " WHERE m.folder = ? AND b.uid IS NULL AND s.uid IS NULL"));
    q.addBindValue(scoped(folder));
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
}

void MailStore::skipBody(const QString &folder, qint64 uid, qint64 size)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO body_skipped (folder, uid, size) VALUES (?, ?, ?)"));
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    q.addBindValue(size);
    q.exec();
}

int MailStore::unskipBodiesUpTo(qint64 maxSize)
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    // maxSize <= 0 means "no limit" — everything previously refused is fair
    // game again, so the backfill picks it up on its next pass.
    if (maxSize <= 0) {
        q.exec(QStringLiteral("DELETE FROM body_skipped"));
    } else {
        q.prepare(QStringLiteral("DELETE FROM body_skipped WHERE size <= ?"));
        q.addBindValue(maxSize);
        q.exec();
    }
    return q.numRowsAffected();
}

QByteArray MailStore::cachedBody(const QString &folder, qint64 uid)
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT raw FROM bodies WHERE folder = ? AND uid = ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    if (q.exec() && q.next())
        return q.value(0).toByteArray();
    return {};
}

void MailStore::storeParts(const QString &folder, qint64 uid, const QList<PartRef> &parts)
{
    storePartsOn(m_db, scoped(folder), uid, parts);
}

void MailStore::storePartsOn(QSqlDatabase &db, const QString &key, qint64 uid,
                             const QList<PartRef> &parts)
{
    if (!db.isOpen() || parts.isEmpty())
        return;
    QSqlQuery att(db);
    // The payload row may already exist from another message referencing the
    // same bytes; only the reference count moves in that case.
    att.prepare(QStringLiteral(
        "INSERT INTO attachments (hash, size, stored, codec, refs, last_used)"
        " VALUES (?, ?, ?, ?, 1, ?)"
        " ON CONFLICT(hash) DO UPDATE SET refs = refs + 1, last_used = excluded.last_used"));
    QSqlQuery part(db);
    part.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO message_parts"
        " (folder, uid, part_id, hash, filename, mime, encoding) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const PartRef &p : parts) {
        att.addBindValue(p.hash);
        att.addBindValue(p.size);
        att.addBindValue(p.stored);
        att.addBindValue(p.codec);
        att.addBindValue(now);
        att.exec();
        part.addBindValue(key);
        part.addBindValue(uid);
        part.addBindValue(p.partId);
        part.addBindValue(p.hash);
        part.addBindValue(p.filename);
        part.addBindValue(p.mime);
        part.addBindValue(QString()); // encoding: payloads are stored decoded
        part.exec();
    }
}

QList<MailStore::PartRef> MailStore::partsFor(const QString &folder, qint64 uid)
{
    QList<PartRef> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT p.part_id, p.hash, p.filename, p.mime, a.size, a.stored, a.codec"
        " FROM message_parts p LEFT JOIN attachments a ON a.hash = p.hash"
        " WHERE p.folder = ? AND p.uid = ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(uid);
    if (!q.exec())
        return out;
    while (q.next()) {
        PartRef p;
        p.partId = q.value(0).toString();
        p.hash = q.value(1).toString();
        p.filename = q.value(2).toString();
        p.mime = q.value(3).toString();
        p.size = q.value(4).toLongLong();
        p.stored = q.value(5).toLongLong();
        p.codec = q.value(6).toInt();
        out.append(p);
    }
    return out;
}

qint64 MailStore::releaseParts(const QString &scopedFolder, const QList<qint64> &uids)
{
    return releasePartsOn(m_db, scopedFolder, uids);
}

qint64 MailStore::releasePartsOn(QSqlDatabase &db, const QString &scopedFolder,
                                 const QList<qint64> &uids)
{
    if (!db.isOpen() || uids.isEmpty())
        return 0;
    QStringList uidList;
    uidList.reserve(uids.size());
    for (qint64 u : uids)
        uidList << QString::number(u);
    const QString uidIn = uidList.join(QLatin1Char(','));

    // Which payloads do these messages reference, and how many times?
    QHash<QString, int> releasing;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT hash FROM message_parts"
                                 " WHERE folder = ? AND uid IN (%1)")
                      .arg(uidIn));
        q.addBindValue(scopedFolder);
        if (q.exec()) {
            while (q.next())
                releasing[q.value(0).toString()] += 1;
        }
    }
    if (releasing.isEmpty())
        return 0;

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM message_parts WHERE folder = ? AND uid IN (%1)")
                    .arg(uidIn));
    del.addBindValue(scopedFolder);
    del.exec();

    qint64 freed = 0;
    QSqlQuery dec(db);
    dec.prepare(QStringLiteral("UPDATE attachments SET refs = refs - ? WHERE hash = ?"));
    QSqlQuery look(db);
    look.prepare(QStringLiteral("SELECT refs, stored FROM attachments WHERE hash = ?"));
    QSqlQuery drop(db);
    drop.prepare(QStringLiteral("DELETE FROM attachments WHERE hash = ?"));
    for (auto it = releasing.cbegin(); it != releasing.cend(); ++it) {
        dec.addBindValue(it.value());
        dec.addBindValue(it.key());
        dec.exec();
        look.addBindValue(it.key());
        if (!look.exec() || !look.next())
            continue;
        if (look.value(0).toLongLong() > 0)
            continue; // still referenced by another message
        freed += look.value(1).toLongLong();
        drop.addBindValue(it.key());
        drop.exec();
        AttachmentStore::remove(it.key());
    }
    return freed;
}

qint64 MailStore::attachmentBytes()
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT IFNULL(SUM(stored), 0) FROM attachments")) || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

bool MailStore::attachmentMigrationPending()
{
    return m_db.isOpen() && !migrationDone(m_db, QStringLiteral("attach_migrate1"));
}

int MailStore::migrateAttachmentsChunk(
    QSqlDatabase &db, qint64 &cursor, int limit, qint64 &bytesSaved,
    const std::function<QByteArray(const QByteArray &, QList<PartRef> *)> &splitFn)
{
    if (!db.isOpen() || limit <= 0)
        return 0;
    // Keyset pagination by rowid: OFFSET would re-walk the whole table on
    // every chunk, and rows are being rewritten underneath us as we go.
    struct Row {
        qint64 rowid;
        QString folder;
        qint64 uid;
        QByteArray raw;
    };
    QList<Row> rows;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT rowid, folder, uid, raw FROM bodies WHERE rowid > ?"
            " ORDER BY rowid LIMIT ?"));
        q.addBindValue(cursor);
        q.addBindValue(limit);
        if (!q.exec())
            return 0;
        while (q.next())
            rows.append({q.value(0).toLongLong(), q.value(1).toString(),
                         q.value(2).toLongLong(), q.value(3).toByteArray()});
    }
    if (rows.isEmpty())
        return 0;

    db.transaction();
    QSqlQuery upd(db);
    upd.prepare(QStringLiteral("UPDATE bodies SET raw = ? WHERE rowid = ?"));
    for (const Row &row : rows) {
        cursor = row.rowid;
        QList<PartRef> parts;
        const QByteArray stub = splitFn(row.raw, &parts);
        if (parts.isEmpty())
            continue; // nothing big enough to lift out
        bytesSaved += row.raw.size() - stub.size();
        upd.addBindValue(stub);
        upd.addBindValue(row.rowid);
        upd.exec();
        storePartsOn(db, row.folder, row.uid, parts);
    }
    QSqlQuery cur(db);
    cur.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO meta_values (key, value) VALUES ('attach_migrate_cursor', ?)"));
    cur.addBindValue(QString::number(cursor));
    cur.exec();
    if (!db.commit())
        db.rollback();
    return rows.size();
}

void MailStore::finishAttachmentMigration(QSqlDatabase &db)
{
    markMigrationDone(db, QStringLiteral("attach_migrate1"));
}

int MailStore::sweepOrphanAttachments()
{
    if (!m_db.isOpen())
        return 0;
    const QStringList onDisk = AttachmentStore::allHashes();
    if (onDisk.isEmpty())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM attachments WHERE hash = ?"));
    int removed = 0;
    for (const QString &hash : onDisk) {
        q.addBindValue(hash);
        if (q.exec() && q.next())
            continue; // known payload
        if (AttachmentStore::remove(hash))
            ++removed;
    }
    return removed;
}

QSqlDatabase MailStore::openWorkerConnection(const QString &name)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                       + QStringLiteral("/mailo.db"));
    if (!db.open()) {
        qWarning() << "mailstore: worker connection failed:" << db.lastError().text();
        return {};
    }
    QSqlQuery pragma(db);
    // Several connections write now; each must wait rather than fail.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=15000"));
    return db;
}

void MailStore::writeBodiesOn(QSqlDatabase &db, const QList<BodyWrite> &batch)
{
    if (!db.isOpen() || batch.isEmpty())
        return;
    db.transaction();
    QSqlQuery body(db);
    body.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO bodies (folder, uid, raw) VALUES (?, ?, ?)"));
    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM fts WHERE rowid ="
        " (SELECT rowid FROM messages WHERE folder = ? AND uid = ?)"));
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
        " SELECT rowid, subject, sender, ?, folder, uid FROM messages"
        " WHERE folder = ? AND uid = ?"));
    QSqlQuery done(db);
    done.prepare(QStringLiteral("DELETE FROM fts_pending WHERE folder = ? AND uid = ?"));
    for (const BodyWrite &w : batch) {
        body.addBindValue(w.scopedFolder);
        body.addBindValue(w.uid);
        body.addBindValue(w.raw);
        body.exec();
        del.addBindValue(w.scopedFolder);
        del.addBindValue(w.uid);
        del.exec();
        ins.addBindValue(w.indexText);
        ins.addBindValue(w.scopedFolder);
        ins.addBindValue(w.uid);
        ins.exec();
        done.addBindValue(w.scopedFolder);
        done.addBindValue(w.uid);
        done.exec();
        // Part rows share the stub's transaction: a payload file with no row
        // is recoverable (the orphan sweep deletes it), a row with no file is
        // not — the message would read back with an empty attachment.
        storePartsOn(db, w.scopedFolder, w.uid, w.parts);
    }
    if (!db.commit())
        db.rollback();
}

void MailStore::removeBodyOnly(const QString &folder, qint64 uid)
{
    if (!m_db.isOpen())
        return;
    const QString key = scoped(folder);
    m_db.transaction();
    releasePartsOn(m_db, key, {uid});
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM bodies WHERE folder = ? AND uid = ?"));
    q.addBindValue(key);
    q.addBindValue(uid);
    q.exec();
    m_db.commit();
}

void MailStore::storeBody(const QString &folder, qint64 uid, const QByteArray &raw,
                          const QString &indexText)
{
    if (!m_db.isOpen() || raw.isEmpty())
        return;
    SlowGuard guard("storeBody");
    m_db.transaction();
    const QString key = scoped(folder);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO bodies (folder, uid, raw) VALUES (?, ?, ?)"));
    q.addBindValue(key);
    q.addBindValue(uid);
    q.addBindValue(raw);
    q.exec();

    if (m_ftsAvailable) {
        // Re-index this message with the body text included (rowid-keyed —
        // filtering fts on folder/uid would scan the whole index).
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral(
            "DELETE FROM fts WHERE rowid ="
            " (SELECT rowid FROM messages WHERE folder = ? AND uid = ?)"));
        del.addBindValue(key);
        del.addBindValue(uid);
        del.exec();
        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral(
            "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
            " SELECT rowid, subject, sender, ?, folder, uid FROM messages"
            " WHERE folder = ? AND uid = ?"));
        ins.addBindValue(indexText);
        ins.addBindValue(key);
        ins.addBindValue(uid);
        ins.exec();
        // Freshly indexed — no background re-index needed anymore.
        QSqlQuery done(m_db);
        done.prepare(QStringLiteral("DELETE FROM fts_pending WHERE folder = ? AND uid = ?"));
        done.addBindValue(key);
        done.addBindValue(uid);
        done.exec();
    }
    m_db.commit();
}

QList<MailStore::PendingBody> MailStore::pendingBodyIndex(int limit)
{
    QList<PendingBody> out;
    if (!m_db.isOpen() || !m_ftsAvailable)
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT b.folder, b.uid, b.raw FROM fts_pending p"
        " JOIN bodies b ON b.folder = p.folder AND b.uid = p.uid LIMIT ?"));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            PendingBody p;
            p.scopedFolder = q.value(0).toString();
            p.uid = q.value(1).toLongLong();
            p.raw = q.value(2).toByteArray();
            out.append(p);
        }
    }
    // Only rows without a cached body can be left once the join comes back
    // empty — they can never be indexed, so drop them and finish the rebuild.
    if (out.isEmpty())
        q.exec(QStringLiteral("DELETE FROM fts_pending"));
    return out;
}

void MailStore::finishBodyIndex(const QString &scopedFolder, qint64 uid,
                                const QString &indexText)
{
    if (!m_db.isOpen())
        return;
    SlowGuard guard("finishBodyIndex");
    m_db.transaction();
    if (m_ftsAvailable) {
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral(
            "DELETE FROM fts WHERE rowid ="
            " (SELECT rowid FROM messages WHERE folder = ? AND uid = ?)"));
        del.addBindValue(scopedFolder);
        del.addBindValue(uid);
        del.exec();
        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral(
            "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
            " SELECT rowid, IFNULL(subject, ''), IFNULL(sender, ''), ?, folder, uid"
            " FROM messages WHERE folder = ? AND uid = ?"));
        ins.addBindValue(indexText);
        ins.addBindValue(scopedFolder);
        ins.addBindValue(uid);
        ins.exec();
    }
    QSqlQuery done(m_db);
    done.prepare(QStringLiteral("DELETE FROM fts_pending WHERE folder = ? AND uid = ?"));
    done.addBindValue(scopedFolder);
    done.addBindValue(uid);
    done.exec();
    m_db.commit();
}

void MailStore::finishBodyIndexBatch(
    const QList<std::tuple<QString, qint64, QString>> &entries)
{
    if (!m_db.isOpen() || entries.isEmpty())
        return;
    SlowGuard guard("finishBodyIndexBatch");
    // One transaction for the whole batch. Per-message commits meant one fsync
    // per cached body — 122k of them on a full index rebuild.
    m_db.transaction();
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral(
        "DELETE FROM fts WHERE rowid ="
        " (SELECT rowid FROM messages WHERE folder = ? AND uid = ?)"));
    QSqlQuery ins(m_db);
    ins.prepare(QStringLiteral(
        "INSERT INTO fts (rowid, subject, sender, body, folder, uid)"
        " SELECT rowid, IFNULL(subject, ''), IFNULL(sender, ''), ?, folder, uid"
        " FROM messages WHERE folder = ? AND uid = ?"));
    QSqlQuery done(m_db);
    done.prepare(QStringLiteral("DELETE FROM fts_pending WHERE folder = ? AND uid = ?"));
    for (const auto &entry : entries) {
        const QString &folder = std::get<0>(entry);
        const qint64 uid = std::get<1>(entry);
        if (m_ftsAvailable) {
            del.addBindValue(folder);
            del.addBindValue(uid);
            del.exec();
            ins.addBindValue(std::get<2>(entry));
            ins.addBindValue(folder);
            ins.addBindValue(uid);
            ins.exec();
        }
        done.addBindValue(folder);
        done.addBindValue(uid);
        done.exec();
    }
    m_db.commit();
}

void MailStore::removeMessages(const QString &folder, const QList<qint64> &uids)
{
    if (!m_db.isOpen() || uids.isEmpty())
        return;
    SlowGuard guard("removeMessages");
    m_db.transaction();
    // Give back the attachment references first: once the part rows are gone
    // the payloads on disk would have no way of ever being freed.
    releaseParts(scoped(folder), uids);
    // fts first (rowid-keyed via messages, which must still exist), then the
    // regular tables.
    if (m_ftsAvailable) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "DELETE FROM fts WHERE rowid ="
            " (SELECT rowid FROM messages WHERE folder = ? AND uid = ?)"));
        for (qint64 uid : uids) {
            q.addBindValue(scoped(folder));
            q.addBindValue(uid);
            q.exec();
        }
    }
    for (const char *table : {"messages", "bodies"}) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE folder = ? AND uid = ?")
                      .arg(QLatin1String(table)));
        for (qint64 uid : uids) {
            q.addBindValue(scoped(folder));
            q.addBindValue(uid);
            q.exec();
        }
    }
    m_db.commit();
}

qint64 MailStore::uidValidity(const QString &folder)
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT uidvalidity FROM account_folders WHERE account = ? AND mailbox = ?"));
    q.addBindValue(m_accountKey);
    q.addBindValue(folder);
    return (q.exec() && q.next()) ? q.value(0).toLongLong() : 0;
}

void MailStore::setUidValidity(const QString &folder, qint64 validity)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE account_folders SET uidvalidity = ? WHERE account = ? AND mailbox = ?"));
    q.addBindValue(validity);
    q.addBindValue(m_accountKey);
    q.addBindValue(folder);
    q.exec();
}

void MailStore::clearFolder(const QString &folder)
{
    if (!m_db.isOpen())
        return;
    SlowGuard guard("clearFolder");
    m_db.transaction();
    QSqlQuery q(m_db);
    // Attachment payloads are refcounted, so the folder's references have to
    // be given back before its part rows go — otherwise the files leak.
    {
        QList<qint64> uids;
        QSqlQuery pick(m_db);
        pick.prepare(QStringLiteral("SELECT uid FROM message_parts WHERE folder = ?"));
        pick.addBindValue(scoped(folder));
        if (pick.exec()) {
            while (pick.next())
                uids.append(pick.value(0).toLongLong());
        }
        releaseParts(scoped(folder), uids);
    }
    // fts first: its rows are found via messages rowids, so the messages
    // rows must still be there.
    if (m_ftsAvailable) {
        q.prepare(QStringLiteral(
            "DELETE FROM fts WHERE rowid IN"
            " (SELECT rowid FROM messages WHERE folder = ?)"));
        q.addBindValue(scoped(folder));
        q.exec();
    }
    for (const char *table : {"messages", "bodies"}) {
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE folder = ?").arg(QLatin1String(table)));
        q.addBindValue(scoped(folder));
        q.exec();
    }
    m_db.commit();
}

int MailStore::purgeChunkOn(QSqlDatabase &db, const QString &key, int limit)
{
    if (!db.isOpen() || key.isEmpty() || limit <= 0)
        return 0;

    // Take one chunk of rowids up front: fts is keyed by messages.rowid, so
    // every delete below is driven by the same fixed set and the three tables
    // cannot drift apart if a later statement fails.
    QList<qint64> rowids;
    QList<qint64> uids;
    {
        QSqlQuery pick(db);
        pick.prepare(QStringLiteral(
            "SELECT rowid, uid FROM messages WHERE folder = ? LIMIT ?"));
        pick.addBindValue(key);
        pick.addBindValue(limit);
        if (pick.exec()) {
            while (pick.next()) {
                rowids.append(pick.value(0).toLongLong());
                uids.append(pick.value(1).toLongLong());
            }
        }
    }

    if (rowids.isEmpty()) {
        // Headers are gone; sweep any bodies left behind (a body can outlive
        // its header if a previous purge was interrupted between the two).
        QSqlQuery rest(db);
        rest.prepare(QStringLiteral(
            "DELETE FROM bodies WHERE rowid IN"
            " (SELECT rowid FROM bodies WHERE folder = ? LIMIT ?)"));
        rest.addBindValue(key);
        rest.addBindValue(limit);
        return (rest.exec() ? rest.numRowsAffected() : 0);
    }

    QStringList rowList;
    rowList.reserve(rowids.size());
    for (qint64 r : std::as_const(rowids))
        rowList << QString::number(r);
    QStringList uidList;
    uidList.reserve(uids.size());
    for (qint64 u : std::as_const(uids))
        uidList << QString::number(u);
    // Numeric ids straight from SQL — no user input, so inlining them (rather
    // than binding N placeholders) is safe and keeps this to three statements.
    const QString rowIn = rowList.join(QLatin1Char(','));
    const QString uidIn = uidList.join(QLatin1Char(','));

    db.transaction();
    // Hand back this chunk's attachment references before its rows go, or the
    // payload files would be orphaned with no row left to free them.
    releasePartsOn(db, key, uids);
    QSqlQuery q(db);
    // fts may be absent on a build without FTS5; the DELETE then simply fails.
    q.exec(QStringLiteral("DELETE FROM fts WHERE rowid IN (%1)").arg(rowIn));
    q.prepare(QStringLiteral("DELETE FROM fts_pending WHERE folder = ? AND uid IN (%1)")
                  .arg(uidIn));
    q.addBindValue(key);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM bodies WHERE folder = ? AND uid IN (%1)").arg(uidIn));
    q.addBindValue(key);
    q.exec();
    q.exec(QStringLiteral("DELETE FROM messages WHERE rowid IN (%1)").arg(rowIn));
    const int removed = q.numRowsAffected();
    if (!db.commit()) {
        db.rollback();
        return 0;
    }
    return removed > 0 ? removed : rowids.size();
}

void MailStore::purgeFolder(const QString &scopedFolder, const QAtomicInt &cancel,
                            const std::function<void(int)> &progress)
{
    const QString name = QStringLiteral("mailstore-purge");
    int total = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           + QStringLiteral("/mailo.db"));
        if (db.open()) {
            QSqlQuery pragma(db);
            // The GUI thread is the other writer. Chunks are small enough that
            // it never waits long, but it must be willing to wait at all.
            pragma.exec(QStringLiteral("PRAGMA busy_timeout=15000"));
            // 100 rows keeps a single write-lock hold to a few ms, so a folder
            // switch on the GUI thread is never stuck behind this.
            while (!cancel.loadRelaxed()) {
                const int removed = purgeChunkOn(db, scopedFolder, 100);
                if (removed <= 0)
                    break;
                total += removed;
                if (progress)
                    progress(total);
                // Yield the write lock between chunks — without this the purge
                // would hold it back-to-back and starve the GUI thread.
                QThread::msleep(20);
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
}

qint64 MailStore::databaseBytes() const
{
    return m_db.isOpen() ? QFileInfo(m_db.databaseName()).size() : 0;
}

qint64 MailStore::reclaimableBytes()
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT * FROM pragma_freelist_count(), pragma_page_size()"))
        || !q.next())
        return 0;
    return q.value(0).toLongLong() * q.value(1).toLongLong();
}

bool MailStore::vacuum(QString *error)
{
    // Its own connection, so this can run on a worker thread while the GUI
    // thread's "mailstore" connection stays put. The connection name is unique
    // per call — a stale one left by a previous failed run would be reused
    // with the wrong thread affinity.
    const QString name = QStringLiteral("mailstore-vacuum");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           + QStringLiteral("/mailo.db"));
        if (!db.open()) {
            if (error)
                *error = db.lastError().text();
        } else {
            QSqlQuery q(db);
            // No busy_timeout would make this fail instantly whenever the GUI
            // thread happens to hold a write lock.
            q.exec(QStringLiteral("PRAGMA busy_timeout=30000"));
            ok = q.exec(QStringLiteral("VACUUM"));
            if (!ok && error)
                *error = q.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool MailStore::headIndicatesAttachment(const QByteArray &head)
{
    static const QRegularExpression ctRe(
        QStringLiteral("(?:^|\\n)content-type:((?:[^\\n]|\\n[ \\t])*)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = ctRe.match(QString::fromLatin1(head));
    return m.hasMatch()
        && m.captured(1).contains(QLatin1String("multipart/mixed"), Qt::CaseInsensitive);
}

bool MailStore::remoteContentAllowedFor(const QString &sender)
{
    if (!m_db.isOpen() || sender.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM remote_senders WHERE sender = ?"));
    q.addBindValue(sender);
    return q.exec() && q.next();
}

void MailStore::setRemoteContentAllowedFor(const QString &sender, bool allowed)
{
    if (!m_db.isOpen() || sender.isEmpty())
        return;
    QSqlQuery q(m_db);
    if (allowed)
        q.prepare(QStringLiteral("INSERT OR IGNORE INTO remote_senders (sender) VALUES (?)"));
    else
        q.prepare(QStringLiteral("DELETE FROM remote_senders WHERE sender = ?"));
    q.addBindValue(sender);
    q.exec();
}

void MailStore::addRecipient(const QString &address, const QString &name)
{
    if (!m_db.isOpen() || m_accountKey.isEmpty() || !address.contains(QLatin1Char('@')))
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO recipients (account, address, name, last_used, use_count)"
        " VALUES (?, ?, ?, ?, 1)"
        " ON CONFLICT(account, address) DO UPDATE SET"
        "  use_count = use_count + 1, last_used = excluded.last_used,"
        "  name = CASE WHEN excluded.name != '' THEN excluded.name ELSE name END"));
    q.addBindValue(m_accountKey);
    q.addBindValue(address.trimmed().toLower());
    q.addBindValue(name.trimmed());
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.exec();
}

QStringList MailStore::recipientCompletions(const QString &prefix, int limit)
{
    QStringList out;
    const QString needle = prefix.trimmed().toLower();
    if (!m_db.isOpen() || m_accountKey.isEmpty() || needle.isEmpty())
        return out;
    QString esc = needle;
    esc.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
        .replace(QLatin1Char('%'), QLatin1String("\\%"))
        .replace(QLatin1Char('_'), QLatin1String("\\_"));
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT address FROM recipients WHERE account = ?"
        " AND (address LIKE ? ESCAPE '\\' OR lower(name) LIKE ? ESCAPE '\\')"
        " ORDER BY use_count DESC, last_used DESC LIMIT ?"));
    q.addBindValue(m_accountKey);
    const QString pattern = QLatin1Char('%') + esc + QLatin1Char('%');
    q.addBindValue(pattern);
    q.addBindValue(pattern);
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next())
            out.append(q.value(0).toString());
    }
    return out;
}

void MailStore::harvestSentRecipients(const QString &sentFolder)
{
    if (!m_db.isOpen() || m_accountKey.isEmpty() || sentFolder.isEmpty())
        return;
    const QString flag = QStringLiteral("recipient_harvest\x1f") + m_accountKey;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta_flags (flag TEXT PRIMARY KEY)"));
    q.prepare(QStringLiteral("SELECT 1 FROM meta_flags WHERE flag = ?"));
    q.addBindValue(flag);
    if (q.exec() && q.next())
        return;

    // Pull To/Cc addresses out of the raw heads with a plain regex — good
    // enough for seeding, and avoids a full MIME parse per cached body.
    static const QRegularExpression addrRe(
        QStringLiteral("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"));
    QSqlQuery bodies(m_db);
    bodies.prepare(QStringLiteral("SELECT raw FROM bodies WHERE folder = ?"));
    bodies.addBindValue(scoped(sentFolder));
    m_db.transaction();
    if (bodies.exec()) {
        while (bodies.next()) {
            const QByteArray raw = bodies.value(0).toByteArray();
            int headEnd = raw.indexOf("\r\n\r\n");
            if (headEnd < 0)
                headEnd = raw.indexOf("\n\n");
            QString head = QString::fromUtf8(headEnd > 0 ? raw.left(headEnd) : raw);
            // Unfold, then keep only the To/Cc header lines.
            head.replace(QRegularExpression(QStringLiteral("\r?\n[ \t]+")),
                         QStringLiteral(" "));
            const QStringList lines =
                head.split(QRegularExpression(QStringLiteral("\r?\n")));
            for (const QString &line : lines) {
                if (!line.startsWith(QLatin1String("To:"), Qt::CaseInsensitive)
                    && !line.startsWith(QLatin1String("Cc:"), Qt::CaseInsensitive))
                    continue;
                auto it = addrRe.globalMatch(line);
                while (it.hasNext())
                    addRecipient(it.next().captured(0));
            }
        }
    }
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO meta_flags (flag) VALUES (?)"));
    q.addBindValue(flag);
    q.exec();
    m_db.commit();
}

QList<MessageListModel::Header> MailStore::search(const QString &folder,
                                                  const QString &keyword)
{
    QList<MessageListModel::Header> out;
    if (!m_db.isOpen() || keyword.trimmed().isEmpty())
        return out;
    SlowGuard guard("search");
    const QString key = scoped(folder);
    QSet<qint64> seen;

    const auto readRows = [&out, &seen](QSqlQuery &q) {
        while (q.next()) {
            MessageListModel::Header h;
            h.uid = q.value(0).toLongLong();
            if (seen.contains(h.uid))
                continue;
            seen.insert(h.uid);
            h.subject = q.value(1).toString();
            h.from = q.value(2).toString();
            h.date = QDateTime::fromSecsSinceEpoch(q.value(3).toLongLong());
            h.seen = q.value(4).toBool();
            h.suspicious = q.value(5).toBool();
            h.authInfo = q.value(6).toString();
            h.attachKind = q.value(7).toInt();
            h.colorLabel = q.value(8).toInt();
            out.append(h);
        }
    };

    if (m_ftsAvailable) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "SELECT m.uid, m.subject, m.sender, m.date, m.seen, m.suspicious, m.auth, m.attach,"
            " m.color FROM messages m JOIN fts f ON m.rowid = f.rowid"
            " WHERE fts MATCH ? AND m.folder = ? ORDER BY m.date DESC LIMIT 200"));
        // Quote as a literal phrase so FTS5 operators in user input can't
        // break it; the trailing * makes it a prefix query, so partial words
        // match too ("hung" finds "hungarian").
        QString phrase = keyword;
        phrase.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        q.addBindValue(QStringLiteral("\"%1\"*").arg(phrase));
        q.addBindValue(key);
        if (q.exec())
            readRows(q);
        else
            qWarning() << "mailstore: fts search failed:" << q.lastError().text();
    }

    // Substring pass over subject/sender — catches word-internal fragments
    // the token-based FTS index cannot ("gari" inside "hungarian").
    QSqlQuery like(m_db);
    like.prepare(QStringLiteral(
        "SELECT uid, subject, sender, date, seen, suspicious, auth, attach, color FROM messages"
        " WHERE folder = ? AND (subject LIKE ? ESCAPE '\\' OR sender LIKE ? ESCAPE '\\')"
        " ORDER BY date DESC LIMIT 200"));
    QString escaped = keyword;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    const QString pattern = QLatin1Char('%') + escaped + QLatin1Char('%');
    like.addBindValue(key);
    like.addBindValue(pattern);
    like.addBindValue(pattern);
    if (like.exec())
        readRows(like);
    return out;
}
