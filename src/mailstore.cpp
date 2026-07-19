#include "mailstore.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

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

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
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
    q.exec(QStringLiteral("DELETE FROM messages WHERE uid <= 0"
                          " OR (IFNULL(subject,'') = '' AND IFNULL(sender,'') = ''"
                          "     AND IFNULL(date,0) <= 0)"));
    q.exec(QStringLiteral("DELETE FROM bodies WHERE uid <= 0"));

    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS folders ("
                          " mailbox TEXT PRIMARY KEY, sortkey INTEGER)"));

    // Per-account folder lists (the legacy global "folders" table is adopted
    // into this one on first run, see adoptLegacyCache()).
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS account_folders ("
                          " account TEXT NOT NULL, mailbox TEXT NOT NULL,"
                          " sortkey INTEGER, uidvalidity INTEGER DEFAULT 0,"
                          " PRIMARY KEY(account, mailbox))"));

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
    m_db.commit();
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
    q.prepare(QStringLiteral("SELECT uid, subject, sender, date, seen, suspicious, auth, attach"
                             " FROM messages WHERE folder = ?"
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
    q.prepare(QStringLiteral("SELECT uid, subject, sender, date, seen, suspicious, auth, attach"
                             " FROM messages WHERE folder = ?"
                             " AND (date < ? OR (date = ? AND uid < ?))"
                             " ORDER BY date DESC, uid DESC LIMIT ?"));
    q.addBindValue(scoped(folder));
    q.addBindValue(dateSecs);
    q.addBindValue(dateSecs);
    q.addBindValue(uid);
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
        " seen = excluded.seen, suspicious = excluded.suspicious, auth = excluded.auth,"
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

QList<qint64> MailStore::uidsWithoutBody(const QString &folder, int limit)
{
    QList<qint64> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT m.uid FROM messages m"
        " LEFT JOIN bodies b ON b.folder = m.folder AND b.uid = m.uid"
        " WHERE m.folder = ? AND b.uid IS NULL"
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
        " WHERE m.folder = ? AND b.uid IS NULL"));
    q.addBindValue(scoped(folder));
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
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

void MailStore::removeMessages(const QString &folder, const QList<qint64> &uids)
{
    if (!m_db.isOpen() || uids.isEmpty())
        return;
    SlowGuard guard("removeMessages");
    m_db.transaction();
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
            out.append(h);
        }
    };

    if (m_ftsAvailable) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "SELECT m.uid, m.subject, m.sender, m.date, m.seen, m.suspicious, m.auth, m.attach"
            " FROM messages m JOIN fts f ON m.rowid = f.rowid"
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
        "SELECT uid, subject, sender, date, seen, suspicious, auth, attach FROM messages"
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
