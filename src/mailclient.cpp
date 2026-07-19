#include "mailclient.h"
#include "oauthhelper.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QLocale>
#include <QSettings>
#include <QTimer>

#include <kimap/appendjob.h>
#include <kimap/capabilitiesjob.h>
#include <kimap/expungejob.h>
#include <kimap/fetchjob.h>
#include <kimap/idlejob.h>
#include <kimap/listjob.h>
#include <kimap/loginjob.h>
#include <kimap/logoutjob.h>
#include <kimap/movejob.h>
#include <kimap/searchjob.h>
#include <kimap/selectjob.h>
#include <kimap/session.h>
#include <kimap/storejob.h>

#include <kmime/content.h>
#include <kmime/message.h>
#include <kmime/util.h>

#include <qt6keychain/keychain.h>

#include <ksmtp/loginjob.h>
#include <ksmtp/sendjob.h>
#include <ksmtp/session.h>

#include <QMimeDatabase>

#include <QTextDocumentFragment>

#include "viewersecurity.h"

#include <algorithm>

/// Depth-first search of the whole MIME tree — mainBodyPart() misses parts
/// nested in structures like multipart/mixed → multipart/related → text/html.
static KMime::Content *findPartByType(KMime::Content *root, const char *mimeType)
{
    if (const auto *ct = std::as_const(*root).contentType(); ct && ct->isMimeType(mimeType))
        return root;
    const auto children = root->contents();
    for (KMime::Content *child : children) {
        if (KMime::Content *found = findPartByType(child, mimeType))
            return found;
    }
    return nullptr;
}

static QSettings appSettings()
{
    return QSettings(QStringLiteral("mailo"), QStringLiteral("mailo"));
}

static const auto kWalletService = QStringLiteral("mailo");
static const auto kWalletKey = QStringLiteral("imap-password");

/// First Authentication-Results header stamped by our own receiving server
/// (senders can forge their own AR headers, so foreign authserv-ids are
/// ignored). Empty when the message carries no trusted verdict.
static QString trustedAuthResults(const KMime::Message *msg,
                                  const QString &trustedAuthDomain)
{
    const auto arHeaders = msg->headersByType("Authentication-Results");
    for (const KMime::Headers::Base *ar : arHeaders) {
        const QString value = ar->asUnicodeString();
        // authserv-id is the first token, e.g. "purelymail.com; spf=pass …"
        const QString authservId =
            value.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        if (!trustedAuthDomain.isEmpty() && !authservId.contains(trustedAuthDomain))
            continue; // forged or foreign AR header — ignore
        return value; // first trusted header wins (newest — servers prepend)
    }
    return {};
}

/// A FETCH response entry that can become a real list row. Entries without a
/// uid or with an empty header block (aborted/partial deliveries) would show
/// up as "(no subject), 1970" ghosts and can never be opened.
static bool imapEntryUsable(const KIMAP::Message &m)
{
    return m.message && m.uid > 0 && !m.message->head().isEmpty();
}

/// Builds a list header from a KIMAP delivery, including the sender-
/// authentication verdict our receiving server stamped into the message.
static MessageListModel::Header headerFromImap(const KIMAP::Message &m,
                                               const QString &trustedAuthDomain)
{
    MessageListModel::Header h;
    h.uid = m.uid;
    const KMime::Message *msg = m.message.get();
    if (const auto *subject = msg->subject())
        h.subject = subject->asUnicodeString();
    if (const auto *from = msg->from())
        h.from = from->asUnicodeString();
    if (const auto *date = msg->date())
        h.date = date->dateTime();
    for (const QByteArray &flag : m.flags) {
        if (flag.compare("\\Seen", Qt::CaseInsensitive) == 0)
            h.seen = true;
    }
    // Header-only heuristic: real attachments arrive as multipart/mixed.
    // Must inspect the raw head — KMime's parsed Content-Type reports
    // text/plain for a multipart message that has no body parts yet.
    h.attachKind = MailStore::headIndicatesAttachment(msg->head())
        ? MessageListModel::GenericAttachment
        : MessageListModel::NoAttachment;
    h.authInfo = trustedAuthResults(m.message.get(), trustedAuthDomain);
    if (!h.authInfo.isEmpty()) {
        static const QRegularExpression verdictRe(
            QStringLiteral("\\b(spf|dkim|dmarc)=([a-z]+)"));
        auto it = verdictRe.globalMatch(h.authInfo.toLower());
        while (it.hasNext()) {
            const QString verdict = it.next().captured(2);
            // fail, softfail, hardfail, permerror — anything but pass/neutral/none
            if (verdict.endsWith(QLatin1String("fail"))
                || verdict == QLatin1String("permerror"))
                h.suspicious = true;
        }
    }
    return h;
}

/// "imap.purelymail.com" → "purelymail.com"; two-label hosts stay as-is.
static QString authDomainForHost(const QString &host)
{
    const QStringList labels = host.toLower().split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() >= 3)
        return labels.mid(1).join(QLatin1Char('.'));
    return host.toLower();
}

/// "1 message" / "42 messages". Qt only picks plural forms for %n when a
/// translator is installed — without one, "(s)"-style source strings leak
/// into the UI verbatim.
static QString countNoun(qint64 n, const char *singular, const char *plural)
{
    return QStringLiteral("%1 %2").arg(n).arg(
        QLatin1String(n == 1 ? singular : plural));
}

MailClient::MailClient(QObject *parent)
    : QObject(parent)
{
    loadAccount();
    m_folderModel.setAccountKey(accountKey());
    m_store.open();
    // Claim any pre-multi-account cache rows for the account they were
    // written by (the one active at upgrade time), then scope everything.
    m_store.adoptLegacyCache(accountKey());
    m_store.setAccountKey(accountKey());

    // Instant startup from cache: folders and INBOX appear before (and
    // without) any network connection.
    loadCachedFolderModel();
    m_selectedFolder = QStringLiteral("INBOX");
    const auto cachedInbox = m_store.cachedHeaders(m_selectedFolder);
    updatePageAnchor(cachedInbox);
    if (!cachedInbox.isEmpty()) {
        m_messageModel.setHeaders(cachedInbox);
        setStatus(tr("INBOX — %1 cached.").arg(countNoun(cachedInbox.size(), "message", "messages")));
    }

    // Keepalive ping so the server doesn't drop us as an idle connection.
    m_keepAlive.setInterval(3 * 60 * 1000);
    connect(&m_keepAlive, &QTimer::timeout, this, [this] {
        if (m_connected && m_session)
            (new KIMAP::CapabilitiesJob(m_session))->start();
    });

    // Fallback poll of the open folder for servers where IDLE push is not
    // running; a no-op whenever the IDLE job is alive.
    m_refreshMinutes = qBound(
        0, appSettings().value(QStringLiteral("ui/refreshMinutes"), 5).toInt(), 24 * 60);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_connected && !m_idleJob)
            refreshCurrentFolder();
    });
    if (m_refreshMinutes > 0)
        m_pollTimer.start(m_refreshMinutes * 60 * 1000);

    m_dateFormat = appSettings()
                       .value(QStringLiteral("ui/dateFormat"), QStringLiteral("yyyy-MM-dd"))
                       .toString();
    m_messageModel.setDateFormat(m_dateFormat);

    // Idle-time backfill: while the user reads recent mail (or writes one,
    // or does nothing), quietly walk the remaining older header windows and
    // then cache the missing bodies. Headers strictly first, so a freshly
    // added account shows the complete list before any body downloads.
    m_backfillTimer.setSingleShot(true);
    m_backfillTimer.setInterval(4000);
    connect(&m_backfillTimer, &QTimer::timeout, this, [this] {
        if (!m_connected || !m_session || m_searchActive)
            return;
        // Something user-triggered (or a prefetch) is running — retry later.
        if (m_busy || m_headerFetch || m_prefetching || !m_prefetchQueue.isEmpty()) {
            m_backfillTimer.start();
            return;
        }
        if (m_oldestFetchedSeq > 1) {
            m_backfill = true;
            fetchOlderFromServer();
            return;
        }
        backfillBodies();
    });

    // Search-index repair: cached bodies queued for re-indexing (after an
    // FTS rebuild) are processed a couple at a time so the GUI thread never
    // stalls; the timer stops itself once the queue is empty.
    m_reindexTimer.setInterval(300);
    connect(&m_reindexTimer, &QTimer::timeout, this, [this] { reindexPendingBodies(); });
    m_reindexTimer.start();
}

void MailClient::loadCachedFolderModel()
{
    const QStringList cachedFolders = m_store.cachedFolders(accountKey());
    QList<FolderModel::Folder> folders;
    for (const QString &mailBox : cachedFolders) {
        FolderModel::Folder f;
        f.mailBox = mailBox;
        const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                             : QLatin1Char('.');
        f.level = int(mailBox.count(sep));
        f.displayName = f.level > 0 ? mailBox.section(sep, -1) : mailBox;
        folders.append(f);
    }
    if (!folders.isEmpty())
        m_folderModel.setFolders(folders);
}

void MailClient::setRefreshMinutes(int minutes)
{
    minutes = qBound(0, minutes, 24 * 60);
    if (m_refreshMinutes == minutes)
        return;
    m_refreshMinutes = minutes;
    appSettings().setValue(QStringLiteral("ui/refreshMinutes"), minutes);
    if (minutes > 0)
        m_pollTimer.start(minutes * 60 * 1000);
    else
        m_pollTimer.stop();
    Q_EMIT refreshMinutesChanged();
}

void MailClient::setDateFormat(const QString &format)
{
    if (m_dateFormat == format || format.isEmpty())
        return;
    m_dateFormat = format;
    appSettings().setValue(QStringLiteral("ui/dateFormat"), format);
    m_messageModel.setDateFormat(format);
    Q_EMIT dateFormatChanged();
}

QVariantList MailClient::cachedFolderList(int index)
{
    QSettings s = appSettings();
    QString user, host;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        user = s.value(QStringLiteral("user")).toString();
        host = s.value(QStringLiteral("host")).toString();
    }
    s.endArray();
    if (host.isEmpty() && user.isEmpty())
        return {};

    QVariantList out;
    const QString key = user + QLatin1Char('@') + host;
    const QSet<QString> collapsed = FolderModel::savedCollapsed(key);
    const QStringList boxes = m_store.cachedFolders(key);
    // Levels of ALL rows up front: hasChildren must see the next row even
    // when a collapsed ancestor hides it from the output.
    QList<int> levels;
    levels.reserve(boxes.size());
    for (const QString &mailBox : boxes) {
        const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                             : QLatin1Char('.');
        levels.append(int(mailBox.count(sep)));
    }
    int skipDeeperThan = -1; // hide rows below a collapsed ancestor
    for (int i = 0; i < boxes.size(); ++i) {
        const QString &mailBox = boxes.at(i);
        const int level = levels.at(i);
        if (skipDeeperThan >= 0 && level > skipDeeperThan)
            continue;
        const bool isCollapsed = collapsed.contains(mailBox);
        skipDeeperThan = isCollapsed ? level : -1;
        const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                             : QLatin1Char('.');
        out.append(QVariantMap{
            {QStringLiteral("name"), level > 0 ? mailBox.section(sep, -1) : mailBox},
            {QStringLiteral("mailBox"), mailBox},
            {QStringLiteral("level"), level},
            {QStringLiteral("hasChildren"),
             i + 1 < boxes.size() && levels.at(i + 1) > level},
            {QStringLiteral("expanded"), !isCollapsed},
        });
    }
    return out;
}

void MailClient::toggleCachedCollapsed(int index, const QString &mailBox)
{
    QSettings s = appSettings();
    QString user, host;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        user = s.value(QStringLiteral("user")).toString();
        host = s.value(QStringLiteral("host")).toString();
    }
    s.endArray();
    if (host.isEmpty() && user.isEmpty())
        return;
    FolderModel::toggleSavedCollapsed(user + QLatin1Char('@') + host, mailBox);
    ++m_cachedFolderRevision;
    Q_EMIT cachedFoldersChanged();
}

void MailClient::openFolderInAccount(int index, const QString &mailBox)
{
    if (index == m_currentAccount) {
        openFolder(mailBox);
        return;
    }
    switchAccountInternal(index, QString());
    // Opened once the new account's connection has listed its folders.
    m_pendingFolder = mailBox;
}

bool MailClient::hasAccount() const
{
    return !m_host.isEmpty() && !m_user.isEmpty();
}

static QString walletKeyFor(const QString &user, const QString &host)
{
    return kWalletKey + QLatin1Char(':') + user + QLatin1Char('@') + host;
}

QString MailClient::walletKey() const
{
    return walletKeyFor(m_user, m_host);
}

/// One-time migration of the legacy single-account keys ("account/…",
/// "smtp/…") into slot 0 of the accounts array.
static void migrateLegacyAccount(QSettings &s)
{
    if (s.value(QStringLiteral("accounts/size"), 0).toInt() > 0)
        return;
    const QString host = s.value(QStringLiteral("account/host")).toString();
    const QString user = s.value(QStringLiteral("account/user")).toString();
    if (host.isEmpty() && user.isEmpty())
        return;

    s.beginWriteArray(QStringLiteral("accounts"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("host"), host);
    s.setValue(QStringLiteral("port"), s.value(QStringLiteral("account/port"), 993));
    s.setValue(QStringLiteral("security"),
               s.value(QStringLiteral("account/security"), int(MailClient::SslTls)));
    s.setValue(QStringLiteral("user"), user);
    s.setValue(QStringLiteral("smtpHost"), s.value(QStringLiteral("smtp/host")));
    s.setValue(QStringLiteral("smtpPort"), s.value(QStringLiteral("smtp/port"), 587));
    s.setValue(QStringLiteral("smtpSecurity"), s.value(QStringLiteral("smtp/security"), 1));
    s.endArray();
    s.setValue(QStringLiteral("currentAccount"), 0);
    s.remove(QStringLiteral("account/host"));
    s.remove(QStringLiteral("account/port"));
    s.remove(QStringLiteral("account/security"));
    s.remove(QStringLiteral("account/user"));
    s.remove(QStringLiteral("smtp"));
    // "account/secret" (pre-wallet plaintext) is handled by loadAccount below.
}

void MailClient::loadAccount()
{
    QSettings s = appSettings();
    migrateLegacyAccount(s);
    m_currentAccount = s.value(QStringLiteral("currentAccount"), 0).toInt();
    loadAccountFields();

    // One-time migration: pre-wallet builds kept the password base64-encoded
    // in the config file. Move it into the system wallet and wipe it.
    const QByteArray legacy = s.value(QStringLiteral("account/secret")).toByteArray();
    if (!legacy.isEmpty()) {
        m_password = QString::fromUtf8(QByteArray::fromBase64(legacy));
        m_secretReady = true;
        writeSecretToWallet();
        return;
    }
    readWalletPassword();
}

/// Reads the active account's config fields (no password) from the array.
void MailClient::loadAccountFields()
{
    QSettings s = appSettings();
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (count == 0) {
        s.endArray();
        m_host.clear();
        m_user.clear();
        m_smtpHost.clear();
        m_signature.clear();
        return;
    }
    m_currentAccount = qBound(0, m_currentAccount, count - 1);
    s.setArrayIndex(m_currentAccount);
    m_host = s.value(QStringLiteral("host")).toString();
    m_port = s.value(QStringLiteral("port"), 993).toInt();
    m_security = s.value(QStringLiteral("security"), int(SslTls)).toInt();
    m_user = s.value(QStringLiteral("user")).toString();
    m_smtpHost = s.value(QStringLiteral("smtpHost")).toString();
    m_smtpPort = s.value(QStringLiteral("smtpPort"), 587).toInt();
    m_smtpSecurity = s.value(QStringLiteral("smtpSecurity"), 1).toInt();
    m_authType = s.value(QStringLiteral("authType"), 0).toInt();
    m_clientId = s.value(QStringLiteral("clientId")).toString();
    m_clientSecret = s.value(QStringLiteral("clientSecret")).toString();
    m_signature = s.value(QStringLiteral("signature")).toString();
    s.endArray();
    m_accessToken.clear(); // tokens never survive an account switch
    m_refreshToken.clear();
    if (m_smtpHost.isEmpty() && !m_host.isEmpty()) {
        // Sensible default: imap.example.com → smtp.example.com
        m_smtpHost = m_host;
        m_smtpHost.replace(QRegularExpression(QStringLiteral("^imap")), QStringLiteral("smtp"));
    }
}

QString MailClient::oauthWalletKey() const
{
    return QStringLiteral("oauth-refresh:") + m_user + QLatin1Char('@') + m_host;
}

void MailClient::readWalletPassword()
{
    m_password.clear();
    m_refreshToken.clear();
    m_secretReady = false;
    const int gen = ++m_walletGen;

    auto finish = [this, gen] {
        if (gen != m_walletGen)
            return; // the account changed while we were reading — stale result
        m_secretReady = true;
        if (m_connectWhenReady) {
            m_connectWhenReady = false;
            connectAccount();
        }
    };

    // OAuth accounts keep a refresh token in the wallet instead of a password.
    if (m_authType != 0) {
        auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
        read->setKey(oauthWalletKey());
        connect(read, &QKeychain::Job::finished, this, [this, read, gen, finish] {
            if (gen != m_walletGen)
                return;
            if (!read->error())
                m_refreshToken = read->textData();
            finish();
        });
        read->start();
        return;
    }

    auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
    read->setKey(walletKey());
    connect(read, &QKeychain::Job::finished, this, [this, read, gen, finish] {
        if (gen != m_walletGen)
            return;
        if (!read->error()) {
            m_password = read->textData();
            finish();
            return;
        }
        if (read->error() != QKeychain::EntryNotFound) {
            qWarning() << "mailo: wallet read failed:" << read->errorString();
            finish();
            return;
        }
        // Single-account era stored the password under a fixed key. Read it
        // once and re-store it under the per-account key.
        auto *legacy = new QKeychain::ReadPasswordJob(kWalletService, this);
        legacy->setKey(kWalletKey);
        connect(legacy, &QKeychain::Job::finished, this, [this, legacy, gen, finish] {
            if (gen != m_walletGen)
                return;
            if (!legacy->error()) {
                m_password = legacy->textData();
                writeSecretToWallet();
            }
            finish();
        });
        legacy->start();
    });
    read->start();
}

void MailClient::writeSecretToWallet()
{
    auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
    write->setKey(walletKey());
    write->setTextData(m_password);
    connect(write, &QKeychain::Job::finished, this, [this, write] {
        if (write->error()) {
            Q_EMIT errorOccurred(
                tr("Could not store the password in the system wallet: %1")
                    .arg(write->errorString()));
            return;
        }
        // Only drop the plaintext once the wallet definitely has it.
        appSettings().remove(QStringLiteral("account/secret"));
    });
    write->start();
}

QStringList MailClient::accountNames() const
{
    QSettings s = appSettings();
    QStringList names;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        const QString user = s.value(QStringLiteral("user")).toString();
        names.append(user.isEmpty() ? s.value(QStringLiteral("host")).toString() : user);
    }
    s.endArray();
    return names;
}

QVariantMap MailClient::accountDetails(int index) const
{
    QSettings s = appSettings();
    QVariantMap out;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        out.insert(QStringLiteral("host"), s.value(QStringLiteral("host")).toString());
        out.insert(QStringLiteral("port"), s.value(QStringLiteral("port"), 993).toInt());
        out.insert(QStringLiteral("security"),
                   s.value(QStringLiteral("security"), int(SslTls)).toInt());
        out.insert(QStringLiteral("user"), s.value(QStringLiteral("user")).toString());
        out.insert(QStringLiteral("smtpHost"), s.value(QStringLiteral("smtpHost")).toString());
        out.insert(QStringLiteral("smtpPort"), s.value(QStringLiteral("smtpPort"), 587).toInt());
        out.insert(QStringLiteral("smtpSecurity"),
                   s.value(QStringLiteral("smtpSecurity"), 1).toInt());
        out.insert(QStringLiteral("authType"), s.value(QStringLiteral("authType"), 0).toInt());
        out.insert(QStringLiteral("clientId"), s.value(QStringLiteral("clientId")).toString());
        out.insert(QStringLiteral("clientSecret"),
                   s.value(QStringLiteral("clientSecret")).toString());
        out.insert(QStringLiteral("signature"),
                   s.value(QStringLiteral("signature")).toString());
    }
    s.endArray();
    return out;
}

void MailClient::saveAccountDetails(int index, const QVariantMap &d)
{
    const QString trimmedHost = d.value(QStringLiteral("host")).toString().trimmed();
    const QString trimmedUser = d.value(QStringLiteral("user")).toString().trimmed();
    const QString password = d.value(QStringLiteral("password")).toString();
    const bool savePassword = d.value(QStringLiteral("savePassword"), true).toBool();
    const int authType = d.value(QStringLiteral("authType"), 0).toInt();

    QSettings s = appSettings();
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    s.endArray();
    if (index < 0 || index > count)
        index = count; // append as a new account

    s.beginWriteArray(QStringLiteral("accounts"), qMax(count, index + 1));
    s.setArrayIndex(index);
    s.setValue(QStringLiteral("host"), trimmedHost);
    s.setValue(QStringLiteral("port"), d.value(QStringLiteral("port"), 993).toInt());
    s.setValue(QStringLiteral("security"), d.value(QStringLiteral("security"), 0).toInt());
    s.setValue(QStringLiteral("user"), trimmedUser);
    s.setValue(QStringLiteral("smtpHost"),
               d.value(QStringLiteral("smtpHost")).toString().trimmed());
    s.setValue(QStringLiteral("smtpPort"), d.value(QStringLiteral("smtpPort"), 587).toInt());
    s.setValue(QStringLiteral("smtpSecurity"),
               d.value(QStringLiteral("smtpSecurity"), 1).toInt());
    s.setValue(QStringLiteral("authType"), authType);
    s.setValue(QStringLiteral("clientId"), d.value(QStringLiteral("clientId")).toString());
    s.setValue(QStringLiteral("clientSecret"),
               d.value(QStringLiteral("clientSecret")).toString());
    s.setValue(QStringLiteral("signature"), d.value(QStringLiteral("signature")).toString());
    s.endArray();

    if (authType == 0) {
        if (!password.isEmpty() && savePassword) {
            auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
            write->setKey(walletKeyFor(trimmedUser, trimmedHost));
            write->setTextData(password);
            write->start();
        } else if (!savePassword) {
            auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
            del->setKey(walletKeyFor(trimmedUser, trimmedHost));
            del->start();
        }
    }

    switchAccountInternal(index, authType == 0 ? password : QString());
}

void MailClient::removeAccount(int index)
{
    QSettings s = appSettings();
    QList<QVariantMap> accounts;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        QVariantMap a;
        const QStringList keys = {
            QStringLiteral("host"), QStringLiteral("port"), QStringLiteral("security"),
            QStringLiteral("user"), QStringLiteral("smtpHost"), QStringLiteral("smtpPort"),
            QStringLiteral("smtpSecurity"), QStringLiteral("authType"),
            QStringLiteral("clientId"), QStringLiteral("clientSecret"),
            QStringLiteral("signature")};
        for (const QString &k : keys)
            a.insert(k, s.value(k));
        accounts.append(a);
    }
    s.endArray();
    if (index < 0 || index >= accounts.size())
        return;

    const QString delUser = accounts.at(index).value(QStringLiteral("user")).toString();
    const QString delHost = accounts.at(index).value(QStringLiteral("host")).toString();
    for (const QString &key : {walletKeyFor(delUser, delHost),
                               QStringLiteral("oauth-refresh:") + delUser
                                   + QLatin1Char('@') + delHost}) {
        auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
        del->setKey(key);
        del->start();
    }

    accounts.removeAt(index);
    s.remove(QStringLiteral("accounts"));
    s.beginWriteArray(QStringLiteral("accounts"), accounts.size());
    for (int i = 0; i < accounts.size(); ++i) {
        s.setArrayIndex(i);
        for (auto it = accounts.at(i).constBegin(); it != accounts.at(i).constEnd(); ++it)
            s.setValue(it.key(), it.value());
    }
    s.endArray();

    if (accounts.isEmpty()) {
        teardownSession();
        m_folderModel.setFolders({});
        m_messageModel.clear();
        m_host.clear();
        m_user.clear();
        Q_EMIT accountChanged();
        Q_EMIT accountsChanged();
        return;
    }
    switchAccountInternal(qMin(m_currentAccount, int(accounts.size()) - 1), QString());
}

void MailClient::switchAccount(int index)
{
    switchAccountInternal(index, QString());
}

void MailClient::switchAccountInternal(int index, const QString &sessionPassword)
{
    QSettings s = appSettings();
    s.setValue(QStringLiteral("currentAccount"), index);
    m_currentAccount = index;

    teardownSession();
    m_folderModel.setFolders({});
    m_messageModel.clear();
    m_selectedFolder.clear();
    m_pendingFolder.clear();
    m_oldestFetchedSeq = 0;
    m_searchActive = false;

    loadAccountFields();
    m_folderModel.setAccountKey(accountKey());
    m_store.setAccountKey(accountKey());
    // The switched-to account's sidebar and INBOX come straight from cache;
    // the network refresh merges into them once connected.
    loadCachedFolderModel();
    m_selectedFolder = QStringLiteral("INBOX");
    const auto cachedInbox = m_store.cachedHeaders(m_selectedFolder);
    updatePageAnchor(cachedInbox);
    if (!cachedInbox.isEmpty())
        m_messageModel.setHeaders(cachedInbox);
    if (!sessionPassword.isEmpty()) {
        ++m_walletGen; // cancel any in-flight wallet read
        m_password = sessionPassword;
        m_secretReady = true;
    } else {
        readWalletPassword();
    }

    Q_EMIT accountChanged();
    Q_EMIT accountsChanged();

    if (!hasAccount())
        return;
    if (m_secretReady)
        connectAccount();
    else
        m_connectWhenReady = true;
}

void MailClient::sendMail(const QString &to, const QString &cc, const QString &subject,
                          const QString &html, const QList<QUrl> &attachments)
{
    const bool haveCredential = m_authType != 0 ? !m_accessToken.isEmpty()
                                                : !m_password.isEmpty();
    if (m_smtpHost.isEmpty() || m_user.isEmpty() || !haveCredential) {
        Q_EMIT errorOccurred(tr("SMTP is not configured (check account settings)."));
        return;
    }
    // Defense against header/SMTP-command injection: no CR/LF survives, and
    // every recipient must look like a bare address.
    static const QRegularExpression crlfRe(QStringLiteral("[\\r\\n]"));
    static const QRegularExpression addrRe(
        QStringLiteral("^[^@\\s<>,;\"]+@[^@\\s<>,;\"]+\\.[^@\\s<>,;\"]+$"));
    auto parseAddresses = [this](QString raw, bool *ok) -> QStringList {
        raw.remove(crlfRe);
        QStringList out;
        *ok = true;
        const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString addr = part.trimmed();
            if (!addrRe.match(addr).hasMatch()) {
                Q_EMIT errorOccurred(tr("Invalid recipient address: %1").arg(addr));
                *ok = false;
                return {};
            }
            out.append(addr);
        }
        return out;
    };

    bool ok = false;
    const QStringList toList = parseAddresses(to, &ok);
    if (!ok)
        return;
    if (toList.isEmpty()) {
        Q_EMIT errorOccurred(tr("No recipient given."));
        return;
    }
    const QStringList ccList = parseAddresses(cc, &ok);
    if (!ok)
        return;
    QString cleanSubject = subject;
    cleanSubject.remove(crlfRe);

    // --- Build the MIME message ---
    const QString fromAddr = ownAddress();

    auto msg = std::make_shared<KMime::Message>();
    msg->from()->fromUnicodeString(fromAddr);
    for (const QString &addr : toList)
        msg->to()->fromUnicodeString(msg->to()->asUnicodeString().isEmpty()
                                         ? addr.trimmed()
                                         : msg->to()->asUnicodeString() + QStringLiteral(", ")
                                             + addr.trimmed());
    if (!ccList.isEmpty())
        msg->cc()->fromUnicodeString(ccList.join(QStringLiteral(", ")));
    msg->subject()->fromUnicodeString(cleanSubject);
    msg->date()->setDateTime(QDateTime::currentDateTime());
    msg->messageID()->generate(m_smtpHost.toUtf8());
    msg->userAgent()->fromUnicodeString(QStringLiteral("mailo/0.1"));

    const QString plain = QTextDocumentFragment::fromHtml(html).toPlainText();

    // text + html alternative pair
    auto alternative = std::make_unique<KMime::Content>();
    alternative->contentType()->setMimeType("multipart/alternative");
    alternative->contentType()->setBoundary(KMime::multiPartBoundary());
    {
        auto textPart = std::make_unique<KMime::Content>();
        textPart->contentType()->setMimeType("text/plain");
        textPart->contentType()->setCharset("utf-8");
        textPart->contentTransferEncoding()->setEncoding(KMime::Headers::CEquPr);
        textPart->fromUnicodeString(plain);
        alternative->appendContent(std::move(textPart));

        auto htmlPart = std::make_unique<KMime::Content>();
        htmlPart->contentType()->setMimeType("text/html");
        htmlPart->contentType()->setCharset("utf-8");
        htmlPart->contentTransferEncoding()->setEncoding(KMime::Headers::CEquPr);
        htmlPart->fromUnicodeString(html);
        alternative->appendContent(std::move(htmlPart));
    }

    msg->contentType()->setMimeType("multipart/mixed");
    msg->contentType()->setBoundary(KMime::multiPartBoundary());
    msg->appendContent(std::move(alternative));

    {
        QMimeDatabase mimeDb;
        for (const QUrl &url : attachments) {
            QFile file(url.toLocalFile());
            if (!file.open(QIODevice::ReadOnly)) {
                Q_EMIT errorOccurred(tr("Could not read attachment %1.").arg(url.toLocalFile()));
                return;
            }
            const QString name = QFileInfo(file).fileName();
            auto part = std::make_unique<KMime::Content>();
            part->contentType()->setMimeType(
                mimeDb.mimeTypeForFile(url.toLocalFile()).name().toUtf8());
            part->contentType()->setName(name);
            part->contentDisposition()->setDisposition(KMime::Headers::CDattachment);
            part->contentDisposition()->setFilename(name);
            part->contentTransferEncoding()->setEncoding(KMime::Headers::CEbase64);
            part->setBody(file.readAll());
            msg->appendContent(std::move(part));
        }
    }
    msg->assemble();

    // --- Ship it over SMTP ---
    setBusy(true);
    setStatus(tr("Sending…"));

    auto *session = new KSmtp::Session(m_smtpHost, quint16(m_smtpPort), this);
    switch (m_smtpSecurity) {
    case 0:
        session->setEncryptionMode(KSmtp::Session::TLS);
        break;
    case 2:
        session->setEncryptionMode(KSmtp::Session::Unencrypted);
        break;
    default:
        session->setEncryptionMode(KSmtp::Session::STARTTLS);
        break;
    }

    auto finish = [this, session, msg, toList, ccList](const QString &error) {
        setBusy(false);
        if (error.isEmpty()) {
            setStatus(tr("Message sent."));
            for (const QString &addr : toList + ccList)
                m_store.addRecipient(addr);
            Q_EMIT mailSent();
            appendToSentFolder(msg->encodedContent(KMime::NewlineType::CRLF));
        } else {
            setStatus(tr("Sending failed."));
            Q_EMIT errorOccurred(error);
        }
        session->quit();
        session->deleteLater();
    };

    connect(session, &KSmtp::Session::stateChanged, this,
            [this, session, msg, toList, ccList, fromAddr, finish](KSmtp::Session::State state) {
                if (state != KSmtp::Session::NotAuthenticated)
                    return;
                auto *login = new KSmtp::LoginJob(session);
                login->setUserName(m_user);
                if (m_authType != 0) {
                    login->setPreferedAuthMode(KSmtp::LoginJob::XOAuth2);
                    login->setPassword(m_accessToken);
                } else {
                    login->setPassword(m_password);
                }
                connect(login, &KJob::result, this,
                        [session, msg, toList, ccList, fromAddr, finish](KJob *job) {
                            if (job->error()) {
                                finish(job->errorString());
                                return;
                            }
                            auto *send = new KSmtp::SendJob(session);
                            send->setFrom(fromAddr);
                            send->setTo(toList);
                            if (!ccList.isEmpty())
                                send->setCc(ccList);
                            send->setData(msg->encodedContent(KMime::NewlineType::CRLF));
                            QObject::connect(send, &KJob::result, session, [finish](KJob *job) {
                                finish(job->error() ? job->errorString() : QString());
                            });
                            send->start();
                        });
                login->start();
            });
    session->open();
}

void MailClient::appendToSentFolder(const QByteArray &rawMessage)
{
    if (!m_connected || !m_session) {
        setStatus(tr("Sent — not copied to the Sent folder (IMAP offline)."));
        return;
    }
    if (m_sentFolder.isEmpty()) {
        setStatus(tr("Sent — no Sent folder found on the server to copy into."));
        return;
    }
    auto *append = new KIMAP::AppendJob(m_session);
    append->setMailBox(m_sentFolder);
    append->setContent(rawMessage);
    append->setFlags({QByteArrayLiteral("\\Seen")});
    append->setInternalDate(QDateTime::currentDateTime());
    connect(append, &KJob::result, this, [this](KJob *job) {
        if (job->error()) {
            Q_EMIT errorOccurred(
                tr("Sent, but copying to %1 failed: %2").arg(m_sentFolder, job->errorString()));
        } else {
            setStatus(tr("Message sent and saved to %1.").arg(m_sentFolder));
        }
    });
    append->start();
}

QString MailClient::ownAddress() const
{
    return m_user.contains(QLatin1Char('@'))
        ? m_user
        : m_user + QLatin1Char('@') + m_smtpHost.section(QLatin1Char('.'), 1);
}

QString MailClient::signatureBlock() const
{
    // The signature editor hands us a full QTextDocument HTML page; splicing
    // <html>/<body> tags into another body string confuses the rich-text
    // parser, so cut the fragment out of the body element first.
    if (QTextDocumentFragment::fromHtml(m_signature).toPlainText().trimmed().isEmpty())
        return {};
    QString fragment = m_signature;
    static const QRegularExpression bodyRe(
        QStringLiteral("<body[^>]*>(.*)</body>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    if (const auto m = bodyRe.match(fragment); m.hasMatch())
        fragment = m.captured(1);
    return fragment;
}

QString MailClient::newMessageBody() const
{
    const QString sig = signatureBlock();
    return sig.isEmpty() ? QString() : QStringLiteral("<p><br></p>") + sig;
}

QString MailClient::loadHtmlFile(const QUrl &fileUrl)
{
    QFile file(fileUrl.toLocalFile());
    // A signature is a short snippet — reject anything that clearly isn't.
    constexpr qint64 maxSize = 1 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxSize) {
        Q_EMIT errorOccurred(file.size() > maxSize
                                 ? tr("%1 is too large for a signature (max 1 MB).")
                                       .arg(fileUrl.fileName())
                                 : tr("Could not read %1.").arg(fileUrl.toLocalFile()));
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QVariantMap MailClient::replyData(bool replyAll)
{
    if (!m_currentMessage)
        return {};
    const KMime::Message *msg = m_currentMessage.get();

    // Bare addr-specs only — that is the shape sendMail() accepts back.
    auto addressesOf = [](const auto *header) {
        QStringList out;
        if (!header)
            return out;
        const auto mailboxes = header->mailboxes();
        for (const auto &mb : mailboxes) {
            const QString addr = QString::fromLatin1(mb.address());
            if (addr.contains(QLatin1Char('@')))
                out.append(addr);
        }
        return out;
    };

    // Reply target: Reply-To when the sender set one, else From.
    QStringList to = addressesOf(msg->replyTo());
    if (to.isEmpty())
        to = addressesOf(msg->from());
    to.removeDuplicates();

    // Reply-all: everyone in the original To/Cc except us and the target.
    QStringList cc;
    if (replyAll) {
        QSet<QString> seen{ownAddress().toLower()};
        for (const QString &addr : std::as_const(to))
            seen.insert(addr.toLower());
        const QStringList others = addressesOf(msg->to()) + addressesOf(msg->cc());
        for (const QString &addr : others) {
            if (!seen.contains(addr.toLower())) {
                seen.insert(addr.toLower());
                cc.append(addr);
            }
        }
    }

    QString subject = msg->subject() ? msg->subject()->asUnicodeString() : QString();
    if (!subject.startsWith(QLatin1String("Re:"), Qt::CaseInsensitive))
        subject = QStringLiteral("Re: ") + subject;

    // Quote the plain-text part (or the HTML stripped to text) — the compose
    // editor is a QTextDocument, which would mangle full email HTML anyway.
    QString quoted = m_currentTextBody.isEmpty()
        ? QTextDocumentFragment::fromHtml(m_currentHtmlBody).toPlainText()
        : m_currentTextBody;
    const QString fromDisplay = msg->from() ? msg->from()->asUnicodeString() : QString();
    QString date;
    if (msg->date()) {
        date = msg->date()->dateTime().toLocalTime().toString(
            m_dateFormat + QStringLiteral(" hh:mm"));
    }
    // Signature goes above the quoted original, right under the cursor line.
    const QString body = QStringLiteral("<p><br></p>") + signatureBlock()
        + QStringLiteral("<p>")
        + tr("On %1, %2 wrote:").arg(date, fromDisplay).toHtmlEscaped()
        + QStringLiteral("</p><blockquote>")
        + quoted.trimmed().toHtmlEscaped().replace(QLatin1Char('\n'), QLatin1String("<br>"))
        + QStringLiteral("</blockquote>");

    return {{QStringLiteral("to"), to.join(QStringLiteral(", "))},
            {QStringLiteral("cc"), cc.join(QStringLiteral(", "))},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("body"), body}};
}

QVariantMap MailClient::forwardData()
{
    if (!m_currentMessage)
        return {};
    const KMime::Message *msg = m_currentMessage.get();

    QString subject = msg->subject() ? msg->subject()->asUnicodeString() : QString();
    if (!subject.startsWith(QLatin1String("Fwd:"), Qt::CaseInsensitive)
        && !subject.startsWith(QLatin1String("Fw:"), Qt::CaseInsensitive))
        subject = QStringLiteral("Fwd: ") + subject;

    QString quoted = m_currentTextBody.isEmpty()
        ? QTextDocumentFragment::fromHtml(m_currentHtmlBody).toPlainText()
        : m_currentTextBody;
    const QString from = msg->from() ? msg->from()->asUnicodeString() : QString();
    const QString origTo = msg->to() ? msg->to()->asUnicodeString() : QString();
    const QString origSubject =
        msg->subject() ? msg->subject()->asUnicodeString() : QString();
    QString date;
    if (msg->date()) {
        date = msg->date()->dateTime().toLocalTime().toString(
            m_dateFormat + QStringLiteral(" hh:mm"));
    }

    QString header = tr("---------- Forwarded message ----------");
    header += QStringLiteral("<br>") + tr("From: %1").arg(from.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("Date: %1").arg(date.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("Subject: %1").arg(origSubject.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("To: %1").arg(origTo.toHtmlEscaped());

    // Signature goes above the forwarded block, right under the cursor line.
    const QString body = QStringLiteral("<p><br></p>") + signatureBlock()
        + QStringLiteral("<p>") + header
        + QStringLiteral("</p><blockquote>")
        + quoted.trimmed().toHtmlEscaped().replace(QLatin1Char('\n'), QLatin1String("<br>"))
        + QStringLiteral("</blockquote>");

    return {{QStringLiteral("to"), QString()},
            {QStringLiteral("cc"), QString()},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("body"), body}};
}

QStringList MailClient::recipientSuggestions(const QString &prefix)
{
    return m_store.recipientCompletions(prefix);
}

void MailClient::harvestRecipients(const KMime::Message *msg)
{
    auto add = [this](const auto *header) {
        if (!header)
            return;
        const auto mailboxes = header->mailboxes();
        for (const auto &mb : mailboxes)
            m_store.addRecipient(QString::fromLatin1(mb.address()),
                                 mb.hasName() ? mb.name() : QString());
    };
    add(std::as_const(*msg).to());
    add(std::as_const(*msg).cc());
}

void MailClient::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

void MailClient::setStatus(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    Q_EMIT statusTextChanged();
}

void MailClient::teardownSession()
{
    m_keepAlive.stop();
    m_backfillTimer.stop();
    m_backfill = false;
    m_bodyBackfill = false;
    m_prefetchQueue.clear();
    stopIdle();
    m_syncReady = false;
    m_syncFolder.clear();
    if (m_syncSession) {
        m_syncSession->deleteLater();
        m_syncSession.clear();
    }
    if (m_session) {
        m_session->close();
        m_session->deleteLater();
        m_session = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        Q_EMIT connectedChanged();
    }
    m_selectedFolder.clear();
}

void MailClient::configureLogin(KIMAP::LoginJob *login) const
{
    login->setUserName(m_user);
    if (m_authType != 0) {
        login->setAuthenticationMode(KIMAP::LoginJob::XOAuth2);
        login->setPassword(m_accessToken);
    } else {
        login->setAuthenticationMode(KIMAP::LoginJob::Plain);
        login->setPassword(m_password);
    }
    switch (m_security) {
    case StartTls:
        login->setEncryptionMode(KIMAP::LoginJob::STARTTLS);
        break;
    case None:
        login->setEncryptionMode(KIMAP::LoginJob::Unencrypted);
        break;
    default:
        login->setEncryptionMode(KIMAP::LoginJob::SSLorTLS);
        break;
    }
}

void MailClient::stopIdle()
{
    m_idleJob.clear(); // owned by the session; dies with it
    if (m_idleSession) {
        m_idleSession->deleteLater();
        m_idleSession.clear();
    }
}

/// IMAP IDLE push on a dedicated connection: the server tells us about new
/// mail in the open folder instantly instead of waiting for a manual refresh.
/// Best-effort — any failure just means we fall back to manual refreshes.
void MailClient::startIdle()
{
    stopIdle();
    if (!m_connected || !hasAccount() || m_selectedFolder.isEmpty())
        return;

    m_idleSession = new KIMAP::Session(m_host, quint16(m_port), this);
    auto *login = new KIMAP::LoginJob(m_idleSession);
    configureLogin(login);
    connect(login, &KJob::result, this, [this](KJob *job) {
        if (job->error() || !m_idleSession) {
            qWarning() << "mailo: idle login failed:" << job->errorString();
            return;
        }
        auto *select = new KIMAP::SelectJob(m_idleSession);
        select->setMailBox(m_selectedFolder);
        select->setOpenReadOnly(true);
        connect(select, &KJob::result, this, [this](KJob *job) {
            if (job->error() || !m_idleSession) {
                qWarning() << "mailo: idle select failed:" << job->errorString();
                return;
            }
            auto *idle = new KIMAP::IdleJob(m_idleSession);
            m_idleJob = idle;
            connect(idle, &KIMAP::IdleJob::mailBoxStats, this,
                    [this](KIMAP::IdleJob *, const QString &mailBox, int, int) {
                        if (mailBox == m_selectedFolder)
                            refreshCurrentFolder();
                    });
            connect(idle, &KJob::result, this, [this](KJob *job) {
                // Server ended IDLE (timeout, capability missing, …) — retry
                // later unless we tore the session down ourselves.
                if (job->error())
                    qWarning() << "mailo: idle ended:" << job->errorString();
                if (m_idleSession && m_connected)
                    QTimer::singleShot(30 * 1000, this, [this] { startIdle(); });
            });
            idle->start();
        });
        select->start();
    });
    login->start();
}

/// Third IMAP connection, dedicated to background transfers (header backfill,
/// body prefetch). KIMAP runs jobs on one connection strictly in order, so
/// without it a folder click queues behind whatever multi-second FETCH the
/// backfill has in flight. Best-effort: if the server refuses the extra
/// connection, background work falls back to the main session.
void MailClient::startSyncSession()
{
    if (m_syncSession)
        return;
    m_syncSession = new KIMAP::Session(m_host, quint16(m_port), this);
    const auto drop = [this] {
        m_syncReady = false;
        m_syncFolder.clear();
        if (m_syncSession) {
            m_syncSession->deleteLater();
            m_syncSession.clear();
        }
    };
    connect(m_syncSession, &KIMAP::Session::connectionFailed, this, drop);
    connect(m_syncSession, &KIMAP::Session::connectionLost, this, drop);
    auto *login = new KIMAP::LoginJob(m_syncSession);
    configureLogin(login);
    connect(login, &KJob::result, this, [this, drop](KJob *job) {
        if (job->error() || !m_syncSession) {
            qWarning() << "mailo: sync-session login failed:" << job->errorString();
            drop();
            return;
        }
        m_syncReady = true;
    });
    login->start();
}

void MailClient::withSyncSession(const QString &folder,
                                 const std::function<void(KIMAP::Session *)> &fn)
{
    if (!m_syncSession || !m_syncReady) {
        // The main session has the folder selected only while it is the open
        // one — anything else cannot be served right now.
        fn(m_session && folder == m_selectedFolder ? m_session.data() : nullptr);
        return;
    }
    if (m_syncFolder == folder) {
        fn(m_syncSession.data());
        return;
    }
    auto *select = new KIMAP::SelectJob(m_syncSession);
    select->setMailBox(folder);
    select->setOpenReadOnly(true);
    connect(select, &KJob::result, this, [this, folder, fn](KJob *job) {
        if (job->error() || !m_syncSession) {
            fn(nullptr);
            return;
        }
        m_syncFolder = folder;
        fn(m_syncSession.data());
    });
    select->start();
}

void MailClient::refreshCurrentFolder()
{
    if (!m_connected || !m_session || m_selectedFolder.isEmpty() || m_busy)
        return;
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    select->setOpenReadOnly(true);
    connect(select, &KJob::result, this, [this](KJob *job) {
        if (job->error())
            return;
        auto *select = static_cast<KIMAP::SelectJob *>(job);
        const int count = select->messageCount();
        if (count <= 0)
            return;
        m_folderMessageCount = count;
        // Newest few headers; appendHeaders() dedupes and updates in place.
        fetchHeaders(qMax(qint64(1), qint64(count) - 19), count, true);
    });
    select->start();
}

void MailClient::acquireTokenAndConnect()
{
    if (!m_oauth) {
        m_oauth = new OAuthHelper(this);
        connect(m_oauth, &OAuthHelper::tokensReady, this,
                [this](const QString &accessToken, const QString &refreshToken,
                       const QDateTime &expiry) {
                    m_accessToken = accessToken;
                    m_accessTokenExpiry = expiry;
                    if (!refreshToken.isEmpty() && refreshToken != m_refreshToken) {
                        m_refreshToken = refreshToken;
                        auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
                        write->setKey(oauthWalletKey());
                        write->setTextData(refreshToken);
                        write->start();
                    }
                    connectAccount();
                });
        connect(m_oauth, &OAuthHelper::failed, this, [this](const QString &message) {
            setBusy(false);
            // A dead refresh token would fail forever — drop it so the next
            // attempt goes through the browser again.
            if (!m_refreshToken.isEmpty()) {
                m_refreshToken.clear();
                auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
                del->setKey(oauthWalletKey());
                del->start();
            }
            setStatus(tr("Sign-in failed."));
            Q_EMIT errorOccurred(message);
        });
    }
    const auto provider = OAuthHelper::Provider(m_authType);

    // Built-in desktop-client IDs so sign-in needs no manual setup (same
    // publicly-documented installed-app credentials Thunderbird ships; a
    // clientId in the account config overrides them).
    QString clientId = m_clientId;
    QString clientSecret = m_clientSecret;
    if (clientId.isEmpty()) {
        if (provider == OAuthHelper::Gmail) {
            clientId = QStringLiteral(
                "406964657835-aq8lmia8j95dhl1a2bvharmfk3t1hgqj.apps.googleusercontent.com");
            clientSecret = QStringLiteral("kSmqreRr0qwBWJgbf5Y-PjSU");
        } else {
            clientId = QStringLiteral("9e5f94bc-e8a4-4e73-b8be-63364c29d753");
            clientSecret.clear();
        }
    }

    setBusy(true);
    if (!m_refreshToken.isEmpty()) {
        setStatus(tr("Refreshing sign-in…"));
        m_oauth->refresh(provider, clientId, clientSecret, m_refreshToken);
    } else {
        setStatus(tr("Complete the sign-in in your browser…"));
        m_oauth->authorize(provider, clientId, clientSecret);
    }
}

void MailClient::connectAccount()
{
    if (!hasAccount()) {
        Q_EMIT errorOccurred(tr("No account configured yet."));
        return;
    }
    if (!m_secretReady) {
        // Wallet lookup still in flight — connect as soon as it lands.
        m_connectWhenReady = true;
        setStatus(tr("Waiting for the system wallet…"));
        return;
    }
    if (m_authType != 0) {
        // OAuth: make sure we hold a live access token first.
        if (m_accessToken.isEmpty()
            || m_accessTokenExpiry <= QDateTime::currentDateTimeUtc()) {
            acquireTokenAndConnect();
            return;
        }
    } else if (m_password.isEmpty()) {
        Q_EMIT errorOccurred(tr("No password set for this account."));
        return;
    }

    // Deliberately do NOT clear the folder/message models here: the cached
    // view stays on screen until the fresh server data merges into it —
    // clearing caused seconds of blank panes on every (re)connect.
    teardownSession();

    setBusy(true);
    setStatus(tr("Connecting to %1:%2…").arg(m_host).arg(m_port));

    m_session = new KIMAP::Session(m_host, quint16(m_port), this);
    // No SessionUiProxy is installed on purpose: KIMAP then rejects invalid
    // TLS certificates instead of asking. Surface that case explicitly.
    connect(m_session, &KIMAP::Session::connectionFailed, this, [this] {
        setBusy(false);
        teardownSession();
        setStatus(tr("Connection failed."));
        Q_EMIT errorOccurred(
            tr("Could not establish a secure connection to %1:%2 — wrong host/port, "
               "or the server's TLS certificate was rejected.")
                .arg(m_host)
                .arg(m_port));
    });
    connect(m_session, &KIMAP::Session::connectionLost, this, [this] {
        setBusy(false);
        m_pendingFolder = m_selectedFolder; // restore it after reconnect
        teardownSession();
        setStatus(tr("Connection lost. Reconnecting…"));
        QTimer::singleShot(2000, this, [this] {
            if (!m_connected && hasAccount())
                connectAccount();
        });
    });

    auto *login = new KIMAP::LoginJob(m_session);
    configureLogin(login);

    connect(login, &KJob::result, this, [this](KJob *job) {
        if (job->error()) {
            setBusy(false);
            setStatus(tr("Login failed."));
            Q_EMIT errorOccurred(job->errorString());
            teardownSession();
            return;
        }
        m_connected = true;
        Q_EMIT connectedChanged();
        m_keepAlive.start();
        startSyncSession();
        setStatus(tr("Connected. Loading folders…"));
        listFolders();
    });
    login->start();
}

void MailClient::listFolders()
{
    auto *list = new KIMAP::ListJob(m_session);
    list->setOption(KIMAP::ListJob::IncludeUnsubscribed);

    auto folders = std::make_shared<QList<FolderModel::Folder>>();
    connect(list, &KIMAP::ListJob::mailBoxesReceived, this,
            [this, folders](const QList<KIMAP::MailBoxDescriptor> &descriptors,
                            const QList<QList<QByteArray>> &flagList) {
                for (int i = 0; i < descriptors.size(); ++i) {
                    const auto &d = descriptors.at(i);
                    FolderModel::Folder f;
                    f.mailBox = d.name;
                    const QChar sep = d.separator;
                    f.level = sep.isNull() ? 0 : int(d.name.count(sep));
                    f.displayName = sep.isNull() ? d.name : d.name.section(sep, -1);
                    if (i < flagList.size()) {
                        for (const QByteArray &flag : flagList.at(i)) {
                            if (flag.compare("\\Noselect", Qt::CaseInsensitive) == 0)
                                f.selectable = false;
                            // RFC 6154 special-use: the server tells us where
                            // sent mail belongs.
                            if (flag.compare("\\Sent", Qt::CaseInsensitive) == 0)
                                m_sentFolder = d.name;
                        }
                    }
                    folders->append(f);
                }
            });

    connect(list, &KJob::result, this, [this, folders](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Listing folders failed."));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        std::sort(folders->begin(), folders->end(),
                  [](const FolderModel::Folder &a, const FolderModel::Folder &b) {
                      // INBOX first, then case-insensitive by path
                      const bool ai = a.mailBox.startsWith(QLatin1String("INBOX"), Qt::CaseInsensitive);
                      const bool bi = b.mailBox.startsWith(QLatin1String("INBOX"), Qt::CaseInsensitive);
                      if (ai != bi)
                          return ai;
                      return QString::compare(a.mailBox, b.mailBox, Qt::CaseInsensitive) < 0;
                  });
        // Fallback when the server doesn't advertise \Sent special-use.
        if (m_sentFolder.isEmpty()) {
            static const QStringList sentNames = {
                QStringLiteral("sent"), QStringLiteral("sent messages"),
                QStringLiteral("sent items"), QStringLiteral("sent mail")};
            for (const auto &f : std::as_const(*folders)) {
                if (sentNames.contains(f.displayName.toLower())) {
                    m_sentFolder = f.mailBox;
                    break;
                }
            }
        }
        m_folderModel.setFolders(*folders);
        QStringList names;
        names.reserve(folders->size());
        for (const auto &f : std::as_const(*folders))
            names.append(f.mailBox);
        m_store.storeFolders(accountKey(), names);
        // Seed the compose autocompletion from cached Sent bodies (once per account).
        m_store.harvestSentRecipients(m_sentFolder);
        setStatus(tr("%1.").arg(countNoun(folders->size(), "folder", "folders")));
        const QString target = m_pendingFolder.isEmpty() ? QStringLiteral("INBOX") : m_pendingFolder;
        m_pendingFolder.clear();
        openFolder(target);
    });
    list->start();
}

QString MailClient::trashFolderName() const
{
    static const QStringList trashNames = {
        QStringLiteral("trash"), QStringLiteral("deleted items"),
        QStringLiteral("deleted messages"), QStringLiteral("deleted"),
        QStringLiteral("bin")};
    const QStringList boxes = m_folderModel.allMailBoxes();
    for (const QString &mailBox : boxes) {
        const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                             : QLatin1Char('.');
        if (trashNames.contains(mailBox.section(sep, -1).toLower()))
            return mailBox;
    }
    return {};
}

bool MailClient::isTrashFolder() const
{
    return !m_selectedFolder.isEmpty() && m_selectedFolder == trashFolderName();
}

/// Removes the uids from the visible list and the on-disk cache.
void MailClient::purgeDeleted(const QList<qint64> &uids)
{
    m_messageModel.removeByUids(uids);
    m_store.removeMessages(m_selectedFolder, uids);
}

void MailClient::deleteMessages(const QVariantList &rows)
{
    if (!m_connected || !m_session) {
        Q_EMIT errorOccurred(tr("Not connected — cannot delete messages."));
        return;
    }
    KIMAP::ImapSet set;
    auto uids = std::make_shared<QList<qint64>>();
    for (const QVariant &v : rows) {
        const qint64 uid = m_messageModel.uidAt(v.toInt());
        if (uid > 0) {
            set.add(uid);
            uids->append(uid);
        }
    }
    if (uids->isEmpty())
        return;

    const bool permanent = isTrashFolder();
    const QString trash = trashFolderName();
    if (!permanent && trash.isEmpty()) {
        Q_EMIT errorOccurred(tr("No trash folder found on the server."));
        return;
    }

    setBusy(true);
    setStatus(permanent ? tr("Deleting %1 permanently…").arg(countNoun(uids->size(), "message", "messages"))
                        : tr("Moving %1 to trash…").arg(countNoun(uids->size(), "message", "messages")));

    // The browsing SELECT is read-only (EXAMINE); STORE/MOVE need read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    connect(select, &KJob::result, this, [this, set, uids, permanent, trash](KJob *job) {
        if (job->error()) {
            setBusy(false);
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        if (permanent) {
            auto *store = new KIMAP::StoreJob(m_session);
            store->setUidBased(true);
            store->setSequenceSet(set);
            store->setMode(KIMAP::StoreJob::AppendFlags);
            store->setFlags({QByteArrayLiteral("\\Deleted")});
            connect(store, &KJob::result, this, [this, uids](KJob *job) {
                if (job->error()) {
                    setBusy(false);
                    Q_EMIT errorOccurred(job->errorString());
                    return;
                }
                auto *expunge = new KIMAP::ExpungeJob(m_session);
                connect(expunge, &KJob::result, this, [this, uids](KJob *job) {
                    setBusy(false);
                    if (job->error()) {
                        Q_EMIT errorOccurred(job->errorString());
                        return;
                    }
                    purgeDeleted(*uids);
                    setStatus(tr("%1 deleted permanently.")
                                  .arg(countNoun(uids->size(), "message", "messages")));
                });
                expunge->start();
            });
            store->start();
        } else {
            auto *move = new KIMAP::MoveJob(m_session);
            move->setUidBased(true);
            move->setSequenceSet(set);
            move->setMailBox(trash);
            connect(move, &KJob::result, this, [this, uids](KJob *job) {
                setBusy(false);
                if (job->error()) {
                    Q_EMIT errorOccurred(job->errorString());
                    return;
                }
                purgeDeleted(*uids);
                setStatus(tr("%1 moved to trash.").arg(countNoun(uids->size(), "message", "messages")));
            });
            move->start();
        }
    });
    select->start();
}

void MailClient::openFolder(const QString &mailBox)
{
    m_backfillTimer.stop();
    m_backfill = false;
    m_bodyBackfill = false;
    m_searchActive = false;
    // Queued prefetch uids belong to the folder that queued them.
    m_prefetchQueue.clear();
    m_selectedFolder = mailBox;
    m_messageModel.clear();

    // Show the cache instantly; the network refresh merges into it.
    const auto cached = m_store.cachedHeaders(mailBox);
    updatePageAnchor(cached);
    if (!cached.isEmpty())
        m_messageModel.setHeaders(cached);

    if (!m_connected || !m_session) {
        setStatus(tr("%1 — offline, %2 cached.")
                      .arg(mailBox, countNoun(cached.size(), "message", "messages")));
        return;
    }
    setBusy(true);
    if (!cached.isEmpty())
        setStatus(tr("%1 — %2 cached, refreshing…").arg(mailBox).arg(cached.size()));
    else
        setStatus(tr("Opening %1…").arg(mailBox));

    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(mailBox);
    select->setOpenReadOnly(true);
    // Full-cache numbers from SQL, not from the (row-limited) preview list —
    // the resume point must account for everything ever fetched.
    connect(select, &KJob::result, this,
            [this, mailBox, maxCachedUid = m_store.maxCachedUid(mailBox),
             cachedCount = m_store.cachedHeaderCount(mailBox)](KJob *job) mutable {
        auto *select = static_cast<KIMAP::SelectJob *>(job);
        // The user already clicked another folder while this SELECT was
        // queued — that folder's own flow owns the models and busy state now.
        if (mailBox != m_selectedFolder)
            return;
        if (job->error()) {
            setBusy(false);
            setStatus(tr("Could not open %1.").arg(mailBox));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }

        // UIDVALIDITY tracking: if the server regenerated the mailbox, every
        // cached uid is meaningless — drop the folder's cache and start over.
        const qint64 validity = select->uidValidity();
        const qint64 cachedValidity = m_store.uidValidity(mailBox);
        if (validity > 0 && cachedValidity > 0 && validity != cachedValidity) {
            qWarning() << "mailo: UIDVALIDITY changed for" << mailBox
                       << cachedValidity << "->" << validity << "- clearing cache";
            m_store.clearFolder(mailBox);
            m_messageModel.clear();
            maxCachedUid = 0;
            cachedCount = 0;
        }
        if (validity > 0 && validity != cachedValidity)
            m_store.setUidValidity(mailBox, validity);

        const int count = select->messageCount();
        m_folderMessageCount = count;
        m_oldestFetchedSeq = 0;
        startIdle(); // (re)watch the newly opened folder for pushed mail
        if (count == 0) {
            setBusy(false);
            setStatus(tr("%1 is empty.").arg(mailBox));
            return;
        }
        if (cachedCount > 0 && maxCachedUid > 0) {
            // Resume where we left off: fetch only what is newer than the
            // cache and continue the backfill below it.
            fetchNewerThanCache(maxCachedUid, cachedCount);
        } else {
            // Newest 100 messages by sequence number; older ones on demand.
            fetchHeaders(qMax(qint64(1), qint64(count) - 99), count, false);
        }
    });
    select->start();
}

void MailClient::fetchNewerThanCache(qint64 maxCachedUid, int cachedCount)
{
    const QString folder = m_selectedFolder;
    setStatus(tr("%1 — checking for new mail…").arg(folder));

    auto *fetch = new KIMAP::FetchJob(m_session);
    KIMAP::ImapSet set;
    set.add(KIMAP::ImapInterval(maxCachedUid + 1)); // open end: "uid:*"
    fetch->setSequenceSet(set);
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QString authDomain = authDomainForHost(m_host);
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [headers, authDomain, maxCachedUid](const QMap<qint64, KIMAP::Message> &messages) {
                for (auto it = messages.cbegin(); it != messages.cend(); ++it) {
                    // "uid:*" always returns at least the mailbox's newest
                    // message, even when its uid is below the requested range.
                    if (imapEntryUsable(it.value()) && it.value().uid > maxCachedUid)
                        headers->append(headerFromImap(it.value(), authDomain));
                }
            });

    connect(fetch, &KJob::result, this, [this, headers, cachedCount, folder](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Fetching headers failed."));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_store.storeHeaders(folder, *headers);
        // Cache is updated above either way, but the visible list and the
        // backfill cursor belong to whatever folder is open NOW.
        if (folder != m_selectedFolder || m_searchActive)
            return;
        const auto merged = m_store.cachedHeaders(m_selectedFolder);
        updatePageAnchor(merged);
        m_messageModel.setHeaders(merged);
        Q_EMIT folderRefreshed();
        // Cached block + newly fetched mail occupy the top of the mailbox;
        // everything below is still-unfetched history for the backfill.
        m_oldestFetchedSeq = qMax(
            qint64(1), m_folderMessageCount - cachedCount - headers->size() + 1);
        if (m_oldestFetchedSeq > 1) {
            const qint64 synced = m_folderMessageCount - m_oldestFetchedSeq + 1;
            setStatus(tr("%1 — %2 of %3 messages synced, fetching the rest in "
                         "the background…")
                          .arg(m_selectedFolder)
                          .arg(synced)
                          .arg(m_folderMessageCount));
        } else {
            setStatus(tr("%1 — %2.")
                          .arg(m_selectedFolder,
                               countNoun(m_messageModel.rowCount(), "message", "messages")));
        }
        scheduleBackfill(); // more headers, or the body-caching phase
    });
    fetch->start();
}

void MailClient::scheduleBackfill()
{
    // The timer tick decides what still needs doing: older header windows
    // first, then missing bodies; it stops arming itself when both are done.
    m_backfillTimer.start();
}

void MailClient::backfillBodies()
{
    if (m_selectedFolder.isEmpty())
        return;
    const auto missing = m_store.uidsWithoutBody(m_selectedFolder);
    if (missing.isEmpty()) {
        if (m_bodyBackfill) {
            m_bodyBackfill = false;
            setStatus(tr("%1 — fully synced, all message bodies cached.")
                          .arg(m_selectedFolder));
        }
        return; // nothing left — the timer stays quiet until rescheduled
    }
    m_bodyBackfill = true;
    const int remaining = m_store.missingBodyCount(m_selectedFolder);
    setStatus(remaining == 1
                  ? tr("%1 — caching 1 message body in the background…")
                        .arg(m_selectedFolder)
                  : tr("%1 — caching %2 message bodies in the background…")
                        .arg(m_selectedFolder)
                        .arg(remaining));
    for (qint64 uid : missing) {
        if (!m_prefetchQueue.contains(uid))
            m_prefetchQueue.append(uid);
    }
    processPrefetchQueue();
    scheduleBackfill(); // next batch (or the "done" status) on a later tick
}

void MailClient::updatePageAnchor(const QList<MessageListModel::Header> &page)
{
    if (page.isEmpty()) {
        m_pageDate = 0;
        m_pageUid = 0;
        return;
    }
    // cachedHeaders* returns date DESC — the last row is the oldest shown.
    m_pageDate = page.last().date.toSecsSinceEpoch();
    m_pageUid = page.last().uid;
}

void MailClient::loadMoreMessages()
{
    if (m_searchActive)
        return;
    // Older mail already cached on disk appears instantly, without touching
    // the network; the server is only asked below the end of the cache.
    if (m_pageUid > 0) {
        const auto older =
            m_store.cachedHeadersBefore(m_selectedFolder, m_pageDate, m_pageUid);
        if (!older.isEmpty()) {
            updatePageAnchor(older);
            m_messageModel.appendHeaders(older);
            return;
        }
    }
    fetchOlderFromServer();
}

void MailClient::fetchOlderFromServer()
{
    if (!m_connected || !m_session || m_busy || m_headerFetch
        || m_oldestFetchedSeq <= 1) {
        m_backfill = false;
        return;
    }
    // Background backfill must not flip the busy state — busy is what makes
    // the UI feel blocked, and nothing user-facing is waiting on this.
    if (!m_backfill)
        setBusy(true);
    const qint64 to = m_oldestFetchedSeq - 1;
    fetchHeaders(qMax(qint64(1), to - 99), to, true);
}

void MailClient::fetchHeaders(qint64 fromSeq, qint64 toSeq, bool append)
{
    const QString folder = m_selectedFolder;
    m_headerFetch = true;
    if (m_backfill) {
        // Background sync runs on its own connection so a folder click on the
        // main session never waits behind it.
        withSyncSession(folder,
                        [this, folder, fromSeq, toSeq, append](KIMAP::Session *session) {
                            if (!session) {
                                m_headerFetch = false;
                                m_backfill = false;
                                return;
                            }
                            fetchHeadersOn(session, folder, fromSeq, toSeq, append, true);
                        });
        return;
    }
    fetchHeadersOn(m_session, folder, fromSeq, toSeq, append, false);
}

void MailClient::fetchHeadersOn(KIMAP::Session *session, const QString &folder,
                                qint64 fromSeq, qint64 toSeq, bool append,
                                bool background)
{
    if (background)
        setStatus(tr("%1 — fetching older messages in the background…").arg(folder));
    else
        setStatus(tr("Fetching headers…"));

    auto *fetch = new KIMAP::FetchJob(session);
    fetch->setSequenceSet(KIMAP::ImapSet(fromSeq, toSeq));
    fetch->setUidBased(false);
    KIMAP::FetchJob::FetchScope scope;
    // FullHeaders (not Headers): we need Authentication-Results for the
    // SPF/DKIM/DMARC verdict, which the minimal header set doesn't include.
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QString authDomain = authDomainForHost(m_host);
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [headers, authDomain](const QMap<qint64, KIMAP::Message> &messages) {
                for (auto it = messages.cbegin(); it != messages.cend(); ++it) {
                    if (imapEntryUsable(it.value()))
                        headers->append(headerFromImap(it.value(), authDomain));
                }
            });

    connect(fetch, &KJob::result, this,
            [this, headers, fromSeq, append, folder, background](KJob *job) {
        m_headerFetch = false;
        if (background)
            m_backfill = false;
        else
            setBusy(false);
        if (job->error()) {
            if (!background) {
                setStatus(tr("Fetching headers failed."));
                Q_EMIT errorOccurred(job->errorString());
            }
            return;
        }
        m_store.storeHeaders(folder, *headers);
        // The user may have moved on: results for a folder that is no longer
        // open go to the cache only — never into the visible list or the
        // current folder's backfill cursor.
        if (folder != m_selectedFolder || m_searchActive)
            return;
        // Only ever move the cursor DOWN: a top-window refresh (poll/IDLE)
        // must not make already-fetched history look unfetched again.
        if (m_oldestFetchedSeq == 0 || fromSeq < m_oldestFetchedSeq)
            m_oldestFetchedSeq = fromSeq;
        if (append) {
            const int added = m_messageModel.appendHeaders(*headers); // dedupes by uid
            if (added == 0 && m_oldestFetchedSeq > 1 && !background) {
                // This window was already on screen from the cache — keep
                // walking older windows until something new shows up,
                // otherwise the scroll appears "stuck" at the cached tail.
                loadMoreMessages();
                return;
            }
        } else {
            // Union of fresh fetch + everything cached, so previously
            // scrolled-in older messages stay visible across sessions.
            const auto merged = m_store.cachedHeaders(m_selectedFolder);
            updatePageAnchor(merged);
            m_messageModel.setHeaders(merged);
            Q_EMIT folderRefreshed();
        }
        if (m_oldestFetchedSeq > 1) {
            // More history on the server — keep syncing it while nothing
            // else is going on, telling the user how far along we are.
            const qint64 synced = m_folderMessageCount - m_oldestFetchedSeq + 1;
            setStatus(tr("%1 — %2 of %3 messages synced, fetching the rest in "
                         "the background…")
                          .arg(m_selectedFolder)
                          .arg(synced)
                          .arg(m_folderMessageCount));
        } else if (background) {
            setStatus(tr("%1 — fully synced, %2.")
                          .arg(m_selectedFolder,
                               countNoun(m_messageModel.rowCount(), "message", "messages")));
        } else {
            setStatus(tr("%1 — %2.")
                          .arg(m_selectedFolder,
                               countNoun(m_messageModel.rowCount(), "message", "messages")));
        }
        scheduleBackfill(); // more headers, or the body-caching phase
    });
    fetch->start();
}

void MailClient::searchMessages(const QString &query, int field)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        clearSearch();
        return;
    }
    // While results are shown, end-of-list scrolling must not page unrelated
    // cached folder rows into the result list.
    m_searchActive = true;

    // /pattern/ → client-side regex filter over the already-loaded list
    if (trimmed.size() > 2 && trimmed.startsWith(QLatin1Char('/')) && trimmed.endsWith(QLatin1Char('/'))) {
        const QRegularExpression re(trimmed.mid(1, trimmed.size() - 2),
                                    QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            Q_EMIT errorOccurred(tr("Invalid regular expression: %1").arg(re.errorString()));
            return;
        }
        m_messageModel.applyFilter(re);
        setStatus(tr("%1 in loaded messages.").arg(countNoun(m_messageModel.rowCount(), "match", "matches")));
        return;
    }

    if (!m_connected || !m_session || m_selectedFolder.isEmpty()) {
        Q_EMIT errorOccurred(tr("Not connected."));
        return;
    }

    setBusy(true);
    setStatus(tr("Searching…"));

    KIMAP::Term::SearchKey key = KIMAP::Term::Text;
    switch (field) {
    case 1:
        key = KIMAP::Term::Subject;
        break;
    case 2:
        key = KIMAP::Term::From;
        break;
    case 3:
        key = KIMAP::Term::Body;
        break;
    }

    auto *search = new KIMAP::SearchJob(m_session);
    search->setUidBased(true);
    search->setTerm(KIMAP::Term(key, trimmed));
    connect(search, &KJob::result, this, [this, trimmed](KJob *job) {
        if (job->error()) {
            // Some servers reject SEARCH variants; fall back to local matching.
            qWarning() << "IMAP SEARCH failed:" << job->errorString();
            setBusy(false);
            localKeywordFilter(trimmed,
                               tr("Server search failed (%1)").arg(job->errorString()));
            return;
        }
        QList<qint64> uids = static_cast<KIMAP::SearchJob *>(job)->results();
        if (uids.isEmpty()) {
            setBusy(false);
            localKeywordFilter(trimmed, tr("No server matches"));
            return;
        }
        // Newest 200 hits are plenty for a result list.
        if (uids.size() > 200)
            uids = uids.mid(uids.size() - 200);
        // Merge in local partial-word hits — many servers (Gmail…) match
        // whole words only, so "hung" would otherwise miss "hungarian".
        fetchHeadersByUids(uids, trimmed);
    });
    search->start();
}

void MailClient::localKeywordFilter(const QString &keyword, const QString &reason)
{
    // Full-text index first (covers subjects, senders and cached bodies).
    const auto hits = m_store.search(m_selectedFolder, keyword);
    if (!hits.isEmpty()) {
        m_oldestFetchedSeq = 1; // result list is complete; no load-more
        m_messageModel.setHeaders(hits);
        setStatus(tr("%1 — %2 in local index.").arg(reason, countNoun(hits.size(), "match", "matches")));
        return;
    }
    const QRegularExpression re(QRegularExpression::escape(keyword),
                                QRegularExpression::CaseInsensitiveOption);
    m_messageModel.applyFilter(re);
    setStatus(tr("%1 — %2 in loaded messages.")
                  .arg(reason, countNoun(m_messageModel.rowCount(), "local match", "local matches")));
}

void MailClient::clearSearch()
{
    m_searchActive = false;
    m_messageModel.applyFilter(QRegularExpression());
    if (!m_selectedFolder.isEmpty() && m_connected)
        openFolder(m_selectedFolder);
}

void MailClient::fetchHeadersByUids(const QList<qint64> &uids,
                                    const QString &localMergeKeyword)
{
    setStatus(tr("Fetching results…"));

    KIMAP::ImapSet set;
    for (qint64 uid : uids)
        set.add(uid);

    auto *fetch = new KIMAP::FetchJob(m_session);
    fetch->setSequenceSet(set);
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QString authDomain = authDomainForHost(m_host);
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [headers, authDomain](const QMap<qint64, KIMAP::Message> &messages) {
                for (auto it = messages.cbegin(); it != messages.cend(); ++it) {
                    if (imapEntryUsable(it.value()))
                        headers->append(headerFromImap(it.value(), authDomain));
                }
            });

    connect(fetch, &KJob::result, this, [this, headers, localMergeKeyword](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Fetching results failed."));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_oldestFetchedSeq = 1; // disable load-more while showing results
        m_messageModel.setHeaders(*headers);
        if (!localMergeKeyword.isEmpty())
            m_messageModel.appendHeaders(
                m_store.search(m_selectedFolder, localMergeKeyword));
        setStatus(tr("%1.").arg(countNoun(m_messageModel.rowCount(), "search result", "search results")));
    });
    fetch->start();
}

/// True for MIME parts that carry an iCalendar invite (.ics).
static bool partIsCalendar(KMime::Content *part)
{
    if (const auto *ct = std::as_const(*part).contentType()) {
        const QByteArray mime = ct->mimeType().toLower();
        if (mime == "text/calendar" || mime == "application/ics")
            return true;
    }
    QString name;
    if (const auto *cd = std::as_const(*part).contentDisposition())
        name = cd->filename();
    if (name.isEmpty()) {
        if (const auto *ct = std::as_const(*part).contentType())
            name = ct->name();
    }
    return name.toLower().endsWith(QLatin1String(".ics"));
}

void MailClient::refineAttachKind(const QString &folder, qint64 uid, KMime::Message *msg)
{
    // Refined (calendar icon in the list) only when every attachment is an
    // .ics; a mixed set keeps the head-derived generic flag.
    const auto parts = msg->attachments();
    if (uid < 0 || parts.isEmpty())
        return;
    for (KMime::Content *part : parts) {
        if (!partIsCalendar(part))
            return;
    }
    m_store.setAttachKind(folder, uid, MessageListModel::CalendarAttachment);
    if (folder == m_selectedFolder)
        m_messageModel.setAttachKind(uid, MessageListModel::CalendarAttachment);
}

void MailClient::collectInlineParts(KMime::Content *root)
{
    if (const auto *cid = std::as_const(*root).contentID(); cid && !cid->identifier().isEmpty()) {
        const auto *ct = std::as_const(*root).contentType();
        m_viewerHandler->setInlinePart(QString::fromLatin1(cid->identifier()),
                                       ct ? ct->mimeType() : QByteArray(),
                                       root->decodedBody());
    }
    const auto children = root->contents();
    for (KMime::Content *child : children)
        collectInlineParts(child);
}

void MailClient::collectAttachments(KMime::Content *root)
{
    m_attachmentParts.clear();
    m_attachments.clear();
    const auto parts = root->attachments();
    for (KMime::Content *part : parts) {
        QString name;
        if (const auto *cd = std::as_const(*part).contentDisposition())
            name = cd->filename();
        if (name.isEmpty()) {
            if (const auto *ct = std::as_const(*part).contentType())
                name = ct->name();
        }
        if (name.isEmpty())
            name = tr("attachment %1").arg(m_attachmentParts.size() + 1);

        m_attachmentParts.append(part);
        m_attachments.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("sizeText"), QLocale().formattedDataSize(part->decodedBody().size())},
        });
    }
    Q_EMIT attachmentsChanged();
}

static QByteArray preformattedPage(const QString &content, bool monospace)
{
    return QByteArrayLiteral("<html><head><meta charset=\"utf-8\"></head><body><pre style=\""
                             "white-space:pre-wrap;word-break:break-word;font-family:")
        + (monospace ? QByteArrayLiteral("monospace") : QByteArrayLiteral("sans-serif"))
        + QByteArrayLiteral(";\">") + content.toHtmlEscaped().toUtf8()
        + QByteArrayLiteral("</pre></body></html>");
}

QString MailClient::htmlViewUrl()
{
    if (!m_viewerHandler)
        return {};
    if (m_currentHtmlBody.isEmpty())
        return textViewUrl();
    // Point inline references at our scheme handler — but only actual
    // src/href attributes and CSS url() values, not arbitrary body text.
    QString html = m_currentHtmlBody;
    static const QRegularExpression attrCidRe(
        QStringLiteral("((?:src|href|background)\\s*=\\s*[\"'])cid:"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cssCidRe(
        QStringLiteral("(url\\(\\s*[\"']?)cid:"), QRegularExpression::CaseInsensitiveOption);
    html.replace(attrCidRe, QStringLiteral("\\1mailo:cid/"));
    html.replace(cssCidRe, QStringLiteral("\\1mailo:cid/"));
    return m_viewerHandler->setMessageHtml(QByteArrayLiteral("<meta charset=\"utf-8\">")
                                           + html.toUtf8());
}

QString MailClient::textViewUrl()
{
    if (!m_viewerHandler)
        return {};
    const QString text = m_currentTextBody.isEmpty() ? tr("(this message has no plain-text part)")
                                                     : m_currentTextBody;
    return m_viewerHandler->setMessageHtml(preformattedPage(text, false));
}

QString MailClient::sourceViewUrl()
{
    if (!m_viewerHandler)
        return {};
    // HTML part when there is one; otherwise the complete raw RFC-822
    // message (headers + MIME structure) — the debugging view.
    const QString source = m_currentHtmlBody.isEmpty() ? QString::fromUtf8(m_currentRaw)
                                                       : m_currentHtmlBody;
    return m_viewerHandler->setMessageHtml(preformattedPage(source, true));
}


QString MailClient::attachmentName(int index) const
{
    // Basename only — a hostile filename must not traverse directories.
    const QString name =
        QFileInfo(m_attachments.at(index).toMap().value(QStringLiteral("name")).toString())
            .fileName();
    return name.isEmpty() ? QStringLiteral("attachment") : name;
}

bool MailClient::writeAttachment(int index, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT errorOccurred(tr("Could not write %1: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(m_attachmentParts.at(index)->decodedBody());
    return true;
}

void MailClient::saveAttachment(int index, const QUrl &fileUrl)
{
    if (index < 0 || index >= m_attachmentParts.size())
        return;
    if (writeAttachment(index, fileUrl.toLocalFile()))
        setStatus(tr("Saved %1.").arg(QFileInfo(fileUrl.toLocalFile()).fileName()));
}

bool MailClient::attachmentRisky(int index) const
{
    if (index < 0 || index >= m_attachmentParts.size())
        return false;
    const QString name =
        m_attachments.at(index).toMap().value(QStringLiteral("name")).toString().toLower();
    static const QStringList riskyExtensions = {
        QStringLiteral(".sh"),       QStringLiteral(".bash"),   QStringLiteral(".zsh"),
        QStringLiteral(".run"),      QStringLiteral(".bin"),    QStringLiteral(".appimage"),
        QStringLiteral(".desktop"),  QStringLiteral(".exe"),    QStringLiteral(".msi"),
        QStringLiteral(".bat"),      QStringLiteral(".cmd"),    QStringLiteral(".com"),
        QStringLiteral(".scr"),      QStringLiteral(".jar"),    QStringLiteral(".py"),
        QStringLiteral(".pl"),       QStringLiteral(".ps1"),    QStringLiteral(".vbs"),
        QStringLiteral(".flatpakref")};
    for (const QString &ext : riskyExtensions) {
        if (name.endsWith(ext))
            return true;
    }
    // Also honor what the sender *declared* — a lie either way is suspicious.
    const QByteArray mime = std::as_const(*m_attachmentParts.at(index)).contentType()
        ? std::as_const(*m_attachmentParts.at(index)).contentType()->mimeType().toLower()
        : QByteArray();
    return mime.contains("executable") || mime.contains("x-sharedlib")
        || mime.contains("x-desktop") || mime.contains("shellscript")
        || mime.contains("x-msdownload") || mime.contains("java-archive");
}

void MailClient::openAttachment(int index)
{
    if (index < 0 || index >= m_attachmentParts.size())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/mailo-attachments");
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + attachmentName(index);
    if (!writeAttachment(index, path))
        return;
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        setStatus(tr("Opened %1.").arg(attachmentName(index)));
    else
        Q_EMIT errorOccurred(tr("No application could open %1.").arg(attachmentName(index)));
}

void MailClient::saveAttachmentToDownloads(int index)
{
    if (index < 0 || index >= m_attachmentParts.size())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(dir);

    const QFileInfo info(attachmentName(index));
    QString candidate = dir + QLatin1Char('/') + info.fileName();
    for (int i = 1; QFile::exists(candidate); ++i) {
        const QString suffix = info.completeSuffix().isEmpty()
            ? QString()
            : QLatin1Char('.') + info.completeSuffix();
        candidate = QStringLiteral("%1/%2 (%3)%4").arg(dir, info.baseName()).arg(i).arg(suffix);
    }
    if (writeAttachment(index, candidate))
        setStatus(tr("Saved to %1.").arg(candidate));
}

void MailClient::fetchMessage(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0)
        return;

    // Previously read message → serve from cache, no network needed.
    const QByteArray cachedRaw = m_store.cachedBody(m_selectedFolder, uid);
    if (!cachedRaw.isEmpty()) {
        auto msg = std::make_shared<KMime::Message>();
        msg->setContent(KMime::CRLFtoLF(cachedRaw));
        presentMessage(msg);
        refineAttachKind(m_selectedFolder, uid, msg.get());
        m_messageModel.markSeen(row);
        setStatus(tr("Loaded from cache."));
        // Read-ahead: sequential reading should never wait on the network.
        prefetchMessage(row + 1);
        prefetchMessage(row + 2);
        return;
    }

    if (!m_connected || !m_session) {
        setStatus(tr("Message not cached — connect to load it."));
        return;
    }

    setBusy(true);
    setStatus(tr("Loading message…"));

    auto *fetch = new KIMAP::FetchJob(m_session);
    fetch->setSequenceSet(KIMAP::ImapSet(uid));
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    // Full = headers + body. Content alone omits the top-level RFC-2822
    // headers, which carry the multipart boundary — KMime then can't split
    // the parts and every multipart (HTML) mail comes out empty.
    scope.mode = KIMAP::FetchJob::FetchScope::Full;
    fetch->setScope(scope);

    // Servers may split one message's FETCH attributes across several
    // untagged responses, which KIMAP surfaces as multiple messagesAvailable
    // emissions. Accumulate and keep the delivery that actually has content;
    // process only once the job is done.
    auto found = std::make_shared<std::shared_ptr<KMime::Message>>();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [found](const QMap<qint64, KIMAP::Message> &messages) {
                for (const KIMAP::Message &m : messages) {
                    if (!m.message)
                        continue;
                    const bool hasContent = !m.message->body().isEmpty()
                        || !m.message->contents().isEmpty();
                    if (!*found || hasContent)
                        *found = m.message;
                }
            });

    connect(fetch, &KJob::result, this, [this, row, found](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Loading message failed."));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        if (!*found) {
            setStatus(tr("Message could not be loaded."));
            return;
        }
        presentMessage(*found);
        // Index text: prefer the plain part, else strip the HTML.
        const QString indexText = !m_currentTextBody.isEmpty()
            ? m_currentTextBody
            : QTextDocumentFragment::fromHtml(m_currentHtmlBody).toPlainText();
        m_store.storeBody(m_selectedFolder, m_messageModel.uidAt(row), m_currentRaw, indexText);
        refineAttachKind(m_selectedFolder, m_messageModel.uidAt(row), found->get());
        setStatus({});
        m_messageModel.markSeen(row);
        // Read-ahead: sequential reading should never wait on the network.
        prefetchMessage(row + 1);
        prefetchMessage(row + 2);
    });
    fetch->start();
}

void MailClient::openExternalUrl(const QUrl &url)
{
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("mailto")) {
        Q_EMIT errorOccurred(tr("Refusing to open %1 link externally.").arg(scheme));
        return;
    }
    if (QDesktopServices::openUrl(url))
        setStatus(tr("Opened %1 in browser.").arg(url.host()));
    else
        Q_EMIT errorOccurred(tr("Could not open %1.").arg(url.toString()));
}

void MailClient::setRemoteContentAllowed(bool allow)
{
    // User toggle: remember the choice for this sender.
    qWarning() << "mailo: remote content toggle" << allow
               << "for sender" << m_currentSenderAddress;
    m_store.setRemoteContentAllowedFor(m_currentSenderAddress, allow);
    applyRemoteContentAllowed(allow);
}

void MailClient::applyRemoteContentAllowed(bool allow)
{
    if (m_remoteContentAllowed == allow)
        return;
    m_remoteContentAllowed = allow;
    if (m_viewerHandler)
        m_viewerHandler->setRemoteContentAllowed(allow);
    Q_EMIT remoteContentAllowedChanged();
}

static QString indexTextFor(KMime::Message *msg)
{
    KMime::Content *textPart = msg->mainBodyPart("text/plain");
    if (!textPart)
        textPart = findPartByType(msg, "text/plain");
    if (textPart)
        return textPart->decodedText();
    KMime::Content *htmlPart = msg->mainBodyPart("text/html");
    if (!htmlPart)
        htmlPart = findPartByType(msg, "text/html");
    if (htmlPart)
        return QTextDocumentFragment::fromHtml(htmlPart->decodedText().left(100000))
            .toPlainText();
    return {};
}

void MailClient::reindexPendingBodies()
{
    if (m_busy)
        return; // never compete with user-visible work
    const auto batch = m_store.pendingBodyIndex(2);
    if (batch.isEmpty()) {
        m_reindexTimer.stop();
        return;
    }
    for (const auto &pending : batch) {
        KMime::Message msg;
        msg.setContent(KMime::CRLFtoLF(pending.raw));
        msg.parse();
        m_store.finishBodyIndex(pending.scopedFolder, pending.uid, indexTextFor(&msg));
    }
}

void MailClient::prefetchMessage(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0 || !m_connected || !m_session)
        return;
    if (m_prefetchQueue.contains(uid))
        return;
    if (!m_store.cachedBody(m_selectedFolder, uid).isEmpty())
        return;
    // Newest request first; keep the queue tiny — this is opportunistic.
    m_prefetchQueue.prepend(uid);
    while (m_prefetchQueue.size() > 4)
        m_prefetchQueue.removeLast();
    processPrefetchQueue();
}

void MailClient::processPrefetchQueue()
{
    if (m_prefetching || m_prefetchQueue.isEmpty() || !m_connected || !m_session)
        return;
    const qint64 uid = m_prefetchQueue.takeFirst();
    const QString folder = m_selectedFolder;
    m_prefetching = true;

    // Body downloads can take seconds — run them on the sync connection so
    // they never delay a user's folder switch or message open.
    withSyncSession(folder, [this, folder, uid](KIMAP::Session *session) {
        if (!session) {
            m_prefetching = false;
            return;
        }
        auto *fetch = new KIMAP::FetchJob(session);
        fetch->setSequenceSet(KIMAP::ImapSet(uid));
        fetch->setUidBased(true);
        KIMAP::FetchJob::FetchScope scope;
        scope.mode = KIMAP::FetchJob::FetchScope::Full;
        fetch->setScope(scope);

        auto found = std::make_shared<std::shared_ptr<KMime::Message>>();
        connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
                [found](const QMap<qint64, KIMAP::Message> &messages) {
                    for (const KIMAP::Message &m : messages) {
                        if (!m.message)
                            continue;
                        if (!*found || !m.message->body().isEmpty()
                            || !m.message->contents().isEmpty())
                            *found = m.message;
                    }
                });
        connect(fetch, &KJob::result, this, [this, found, folder, uid](KJob *job) {
            m_prefetching = false;
            if (!job->error() && *found) {
                KMime::Message *msg = found->get();
                if (msg->contents().isEmpty())
                    msg->parse();
                m_store.storeBody(folder, uid, msg->encodedContent(), indexTextFor(msg));
                refineAttachKind(folder, uid, msg);
                if (!m_sentFolder.isEmpty() && folder == m_sentFolder)
                    harvestRecipients(msg);
            }
            processPrefetchQueue();
        });
        fetch->start();
    });
}

void MailClient::presentMessage(const std::shared_ptr<KMime::Message> &message)
{
    KMime::Message *msg = message.get();
    // KIMAP delivers the message already parsed. Calling parse() again on a
    // parsed multipart message DESTROYS it: the body was consumed into the
    // child parts, so a re-parse from the now-empty body drops every part
    // and leaves a headers-only shell. Parse only if it hasn't happened yet.
    if (msg->contents().isEmpty())
        msg->parse();

    // Privacy default: remote content blocked, unless the user previously
    // chose "load remote content" for this exact sender address. Must run
    // AFTER the parse guard — cache-served messages have no headers before it.
    m_currentSenderAddress.clear();
    if (const auto *from = std::as_const(*msg).from(); from && !from->mailboxes().isEmpty())
        m_currentSenderAddress =
            QString::fromLatin1(from->mailboxes().first().address()).toLower();
    const bool remembered = m_store.remoteContentAllowedFor(m_currentSenderAddress);
    qWarning() << "mailo: presenting message from" << m_currentSenderAddress
               << "remembered remote-content" << remembered;
    applyRemoteContentAllowed(remembered);
    // A message in the Sent folder means its To/Cc were once our recipients —
    // feed them to the compose autocompletion.
    if (!m_sentFolder.isEmpty() && m_selectedFolder == m_sentFolder)
        harvestRecipients(msg);
    m_currentMessage = message; // keeps all parts alive
    m_currentRaw = msg->encodedContent();

    KMime::Content *htmlPart = msg->mainBodyPart("text/html");
    if (!htmlPart)
        htmlPart = findPartByType(msg, "text/html");
    KMime::Content *textPart = msg->mainBodyPart("text/plain");
    if (!textPart)
        textPart = findPartByType(msg, "text/plain");
    if (!textPart && !htmlPart)
        textPart = msg->textContent();

    m_currentHtmlBody = htmlPart ? htmlPart->decodedText() : QString();
    m_currentTextBody = textPart ? textPart->decodedText() : QString();

    if (m_currentHtmlBody.isEmpty() && m_currentTextBody.isEmpty()) {
        const auto *ct = std::as_const(*msg).contentType();
        qWarning() << "mailo: no displayable part found. content-type:"
                   << (ct ? ct->mimeType() : QByteArrayLiteral("(none)"))
                   << "children:" << msg->contents().size()
                   << "raw size:" << m_currentRaw.size();
    }

    // Plain-text stand-in shown while Chromium renders the HTML view.
    if (!m_currentTextBody.isEmpty()) {
        m_textPreview = m_currentTextBody;
    } else {
        // Cap the input: stripping hundreds of KB of HTML would defeat the
        // purpose of an *instant* preview.
        m_textPreview =
            QTextDocumentFragment::fromHtml(m_currentHtmlBody.left(100000)).toPlainText();
    }
    Q_EMIT textPreviewChanged();

    if (m_viewerHandler) {
        m_viewerHandler->clearInlineParts();
        collectInlineParts(msg);
    }
    collectAttachments(msg);

    const QString bodyUrl = htmlPart ? htmlViewUrl() : textViewUrl();

    const KMime::Message *cmsg = msg;
    const QString subject = cmsg->subject() ? cmsg->subject()->asUnicodeString() : QString();
    const QString from = cmsg->from() ? cmsg->from()->asUnicodeString() : QString();
    const QString to = cmsg->to() ? cmsg->to()->asUnicodeString() : QString();
    const QString cc = cmsg->cc() ? cmsg->cc()->asUnicodeString() : QString();
    QString date;
    if (cmsg->date()) {
        const QDateTime local = cmsg->date()->dateTime().toLocalTime();
        date = local.date() == QDate::currentDate()
            ? local.toString(QStringLiteral("hh:mm"))
            : local.toString(m_dateFormat + QStringLiteral(" hh:mm"));
    }
    const QString authInfo = trustedAuthResults(cmsg, authDomainForHost(m_host));
    Q_EMIT messageLoaded(subject, from, to, cc, date, bodyUrl, authInfo);
}
