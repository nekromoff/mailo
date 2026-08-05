// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * Checks the backend-neutral message identity added for JMAP (doc/JMAP_ROADMAP.md
 * phase 0): the `remote_id` column on `messages` and the opaque per-folder
 * `sync_state` on `account_folders`.
 *
 * Runs against a throwaway database in its own AppDataLocation — set via
 * QStandardPaths::setTestModeEnabled() plus a test-only application name — so
 * it can never open, let alone write to, the real mail cache.
 */

#include "../src/mailstore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

static int failures = 0;

/// Plain stdout rather than qInfo: a diagnostic tool has to print its findings
/// whatever the ambient QT_LOGGING_RULES say, and the default rules drop
/// qInfo() on the floor.
static QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

static void check(bool ok, const QString &what)
{
    out() << (ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) << what << Qt::endl;
    if (!ok)
        ++failures;
}

static MessageListModel::Header makeHeader(qint64 uid, const QString &subject,
                                           const QString &remoteId)
{
    MessageListModel::Header h;
    h.uid = uid;
    h.subject = subject;
    h.from = QStringLiteral("Someone <someone@example.com>");
    h.date = QDateTime::fromSecsSinceEpoch(1700000000 + uid);
    h.remoteId = remoteId;
    return h;
}

/// The stored header for \a uid, or a default-constructed one when absent.
static MessageListModel::Header find(MailStore &store, const QString &folder, qint64 uid)
{
    const auto rows = store.cachedHeaders(folder);
    for (const auto &h : rows) {
        if (h.uid == uid)
            return h;
    }
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Both together are what redirect AppDataLocation away from the real cache.
    QCoreApplication::setApplicationName(QStringLiteral("mailo-storetest"));
    QCoreApplication::setOrganizationName(QStringLiteral("mailo-storetest"));
    QStandardPaths::setTestModeEnabled(true);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty() || !dir.contains(QLatin1String("mailo-storetest"))) {
        qWarning() << "refusing to run: test data location is not isolated:" << dir;
        return 2;
    }
    // Start from nothing, so a rerun is not judging a previous run's rows.
    QDir(dir).removeRecursively();

    MailStore store;
    if (!store.open()) {
        qWarning() << "cannot open test store";
        return 2;
    }
    const QString account = QStringLiteral("tester@example.net");
    const QString folder = QStringLiteral("INBOX");
    store.setAccountKey(account);
    store.storeFolders(account, {folder});

    out() << "remote_id" << Qt::endl;

    // An IMAP-shaped write: the backend states the uid as its remote id.
    store.storeHeaders(folder, {makeHeader(101, QStringLiteral("with id"),
                                           QStringLiteral("101"))});
    check(find(store, folder, 101).remoteId == QLatin1String("101"),
          QStringLiteral("a written remote id reads back verbatim"));

    // A JMAP-shaped write: an opaque string bearing no relation to the uid.
    store.storeHeaders(folder, {makeHeader(102, QStringLiteral("opaque id"),
                                           QStringLiteral("Mdeadbeef01"))});
    check(find(store, folder, 102).remoteId == QLatin1String("Mdeadbeef01"),
          QStringLiteral("an opaque (non-numeric) remote id survives a round trip"));

    // A producer that does not know the id — the Thunderbird importer, or a
    // row cached before the column existed. The uid stands in on read.
    store.storeHeaders(folder, {makeHeader(103, QStringLiteral("no id"), QString())});
    check(find(store, folder, 103).remoteId == QLatin1String("103"),
          QStringLiteral("a row with no remote id reads back as its uid"));

    // The COALESCE rule: a later write that does not know the id must not
    // erase one already recorded.
    store.storeHeaders(folder, {makeHeader(102, QStringLiteral("refreshed"), QString())});
    const auto refreshed = find(store, folder, 102);
    check(refreshed.subject == QLatin1String("refreshed"),
          QStringLiteral("a header refresh still updates the ordinary columns"));
    check(refreshed.remoteId == QLatin1String("Mdeadbeef01"),
          QStringLiteral("a refresh without a remote id keeps the recorded one"));

    out() << "messagesOlderThan" << Qt::endl;

    // What the spam sweep runs on. It deletes on the server by remote id and
    // forgets locally by uid, so both names have to come back — and the date
    // rule decides what gets deleted at all, which is worth pinning down.
    const QString junk = QStringLiteral("Junk");
    store.storeFolders(account, {folder, junk});
    const qint64 cutoff = 1700000000;
    auto dated = [](qint64 uid, const QString &remoteId, const QDateTime &date) {
        MessageListModel::Header h = makeHeader(uid, QStringLiteral("aged"), remoteId);
        h.date = date;
        return h;
    };
    store.storeHeaders(junk, {
        dated(201, QStringLiteral("Mold01"), QDateTime::fromSecsSinceEpoch(cutoff - 86400)),
        dated(202, QStringLiteral("Mnew01"), QDateTime::fromSecsSinceEpoch(cutoff + 86400)),
        // No usable date — the "1970" rows — and no remote id either.
        dated(203, QString(), QDateTime()),
    });

    const auto aged = store.messagesOlderThan(junk, cutoff);
    QList<qint64> agedUids;
    for (const auto &m : aged)
        agedUids.append(m.uid);
    std::sort(agedUids.begin(), agedUids.end());
    check(agedUids == QList<qint64>({201, 203}),
          QStringLiteral("only messages older than the cutoff, plus the dateless ones"));

    QString oldId, datelessId;
    for (const auto &m : aged) {
        if (m.uid == 201)
            oldId = m.remoteId;
        if (m.uid == 203)
            datelessId = m.remoteId;
    }
    check(oldId == QLatin1String("Mold01"),
          QStringLiteral("an aged message carries its recorded remote id"));
    check(datelessId == QLatin1String("203"),
          QStringLiteral("an aged message with no remote id falls back to its uid"));

    check(store.messagesOlderThan(QStringLiteral("Nonexistent"), cutoff).isEmpty(),
          QStringLiteral("an unknown folder has nothing to sweep"));

    out() << "mark all read" << Qt::endl;

    // What a folder's "mark all read" reads and then writes. The unread rows
    // come back named both ways, because the server is told about them by
    // remote id while the cache forgets them by uid.
    const QString bulk = QStringLiteral("Bulk");
    store.storeFolders(account, {folder, junk, bulk});
    store.storeHeaders(bulk, {
        makeHeader(301, QStringLiteral("unread"), QStringLiteral("Mbulk01")),
        makeHeader(302, QStringLiteral("unread, no id"), QString()),
        makeHeader(303, QStringLiteral("already read"), QStringLiteral("Mbulk03")),
    });
    store.setSeen(bulk, 303);

    auto unseenUids = [&store](const QString &box) {
        QList<qint64> uids;
        for (const auto &m : store.unseenMessages(box))
            uids.append(m.uid);
        std::sort(uids.begin(), uids.end());
        return uids;
    };
    const auto unseen = store.unseenMessages(bulk);
    check(unseenUids(bulk) == QList<qint64>({301, 302}),
          QStringLiteral("only the unread messages of the folder come back"));
    QString withId, withoutId;
    for (const auto &m : unseen) {
        if (m.uid == 301)
            withId = m.remoteId;
        if (m.uid == 302)
            withoutId = m.remoteId;
    }
    check(withId == QLatin1String("Mbulk01"),
          QStringLiteral("an unread message carries its recorded remote id"));
    check(withoutId == QLatin1String("302"),
          QStringLiteral("an unread message with no remote id falls back to its uid"));

    store.setFolderSeen(bulk);
    check(unseenUids(bulk).isEmpty(),
          QStringLiteral("marking the folder read leaves nothing unread"));
    // The neighbouring folder is what a mis-scoped UPDATE would take with it.
    check(unseenUids(junk) == QList<qint64>({201, 202, 203}),
          QStringLiteral("another folder's unread mail is untouched"));

    out() << "sync_state" << Qt::endl;

    check(store.syncState(folder).isEmpty(),
          QStringLiteral("an untouched folder has no sync state"));
    store.setSyncState(folder, QStringLiteral("state-abc-1"));
    check(store.syncState(folder) == QLatin1String("state-abc-1"),
          QStringLiteral("a sync state reads back verbatim"));
    store.setSyncState(folder, QStringLiteral("state-abc-2"));
    check(store.syncState(folder) == QLatin1String("state-abc-2"),
          QStringLiteral("a later sync state replaces the earlier one"));
    check(store.uidValidity(folder) == 0,
          QStringLiteral("writing a sync state leaves uidvalidity alone"));
    store.setUidValidity(folder, 4242);
    check(store.syncState(folder) == QLatin1String("state-abc-2"),
          QStringLiteral("writing uidvalidity leaves the sync state alone"));

    check(store.syncState(QStringLiteral("Nonexistent")).isEmpty(),
          QStringLiteral("an unknown folder has no sync state"));

    if (failures) {
        out() << failures << " check(s) failed" << Qt::endl;
        return 1;
    }
    out() << "all checks passed" << Qt::endl;
    return 0;
}
