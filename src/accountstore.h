// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariantMap>

#include "mailbackend.h"

/**
 * One account's configuration, as it sits in QSettings.
 *
 * Fields only — no persistence and no policy. MailClient holds the active
 * account's copy and reads it everywhere it used to read its own members;
 * AccountStore is what puts values in it.
 */
struct AccountConfig {
    QString host;
    /// MailBackend::Protocol as an int, because it is read straight out of
    /// QSettings and written straight back. Imap for every account that
    /// predates JMAP, which is what makes the key's absence meaningful.
    int protocol = static_cast<int>(MailBackend::Protocol::Imap);
    int port = 993;
    int security = 0; ///< MailClient::SslTls
    QString user;
    /// The address mail is sent from. Separate from user because a login name
    /// need not be an address (and need not share its domain). Empty on
    /// accounts saved before this was a field; MailClient::ownAddress() then
    /// falls back to the old guess rather than forcing everyone through the
    /// account dialog.
    QString email;
    /// The name recipients see in From, e.g. "Jane Roe" <jane@example.com>.
    /// Optional: empty sends a bare address, which is what every account did
    /// before this field existed.
    QString displayName;
    /// Optional Organization: header. Empty sends no such header at all.
    QString organization;
    QString smtpHost;
    int smtpPort = 587;
    int smtpSecurity = 1; ///< 0 TLS, 1 STARTTLS, 2 none
    int authType = 0;     ///< 0 password, 1 Gmail OAuth2, 2 Microsoft OAuth2
    /// The stored secret is an API token, sent as `Authorization: Bearer`,
    /// rather than a password to be offered to the server's own login. JMAP
    /// only: it is how the protocol's servers actually authenticate (Fastmail
    /// hands out API tokens, and Cyrus accepts a JWT and no password at all),
    /// while IMAP has no request to put such a header on. Orthogonal to
    /// authType, which stays 0: there is nothing to refresh and no browser to
    /// open, only a secret the user pasted.
    bool bearerAuth = false;
    QString clientId;
    QString clientSecret;
    QString signature;  ///< per-account signature (HTML, may be a full doc)
    bool htmlMail = true; ///< send multipart text+HTML; false = plain text only
    bool local = false;   ///< local archive: never connect, never sync
    QString cacheKey;     ///< fixed storage key of an imported archive ("" = user@host)

    /// Storage identity: the wallet entry and the on-disk message cache are
    /// filed under this. Deliberately keyed on the login, not the e-mail
    /// address — rebasing it would orphan every cached folder and stored
    /// password on existing installs. Imported archives carry an explicit
    /// cacheKey instead, so filling in server details later (which changes
    /// user/host) does not orphan the imported mail.
    QString accountKey() const
    {
        return cacheKey.isEmpty() ? user + QLatin1Char('@') + host : cacheKey;
    }
    /// Enough of an identity to connect with.
    bool valid() const { return !host.isEmpty() && !user.isEmpty(); }
};

/**
 * Where accounts live: the QSettings "accounts" array, its one-time
 * migrations, and the secrets that go with it in the system wallet.
 *
 * Everything here is about *storage*. Which account is open, when to dial it
 * and what to do with a secret once it arrives are MailClient's questions —
 * this class only answers what is on disk and puts things there.
 */
class AccountStore : public QObject
{
    Q_OBJECT
public:
    explicit AccountStore(QObject *parent = nullptr);

    /// The application's settings handle. One definition, so every reader
    /// opens the same file.
    static QSettings settings();

    // --- reading ---
    int count() const;
    /// Display names for the account pane: the address, falling back to the
    /// login and then the host for accounts that have no address stored.
    QStringList names() const;
    /// Config fields of account \a index as a map; empty for an unknown index.
    QVariantMap details(int index) const;
    /// Storage key of account \a index straight from settings — same rule as
    /// AccountConfig::accountKey(), for the sidebar paths that inspect
    /// non-active accounts. Empty when the index is unknown or the account has
    /// no identity yet.
    static QString storedKeyAt(QSettings &s, int index, bool *local = nullptr);
    QString storedKeyAt(int index, bool *local = nullptr) const;
    /// Every account as a map, in order — what the poll pass walks.
    QList<QVariantMap> all() const;

    int currentIndex() const { return m_current; }
    /// Sets the active index and persists it. Does not load anything.
    void setCurrentIndex(int index);

    // --- loading ---
    /// Runs the one-time migrations and reads the stored current index.
    void migrate();
    /// Reads the active account's config fields (no secret) from the array,
    /// clamping the current index to what actually exists.
    AccountConfig loadFields();

    // --- writing ---
    /// What saveDetails() did, so the caller can decide about the session.
    struct SaveResult {
        int index = -1;   ///< where it landed (an out-of-range index appends)
        bool existed = false;
        /// A setting a live session is built from differs from what was there.
        bool sessionChanged = false;
        QString password; ///< as supplied, for the session that may follow
        int authType = 0;
        /// The four cached-in-MailClient preference fields as written.
        QString displayName;
        QString organization;
        QString signature;
        bool htmlMail = true;
    };
    /// Creates (index out of range) or updates an account from the same map
    /// shape details() returns, plus "password"/"savePassword", and writes the
    /// wallet entry. Decides nothing about the connection.
    SaveResult saveDetails(int index, const QVariantMap &d);
    /// Writes just the keys \a d carries into an existing account, refusing
    /// the session keys. Returns the merged account map when something
    /// actually changed, and an empty map when nothing did.
    QVariantMap savePrefs(int index, const QVariantMap &d);
    /// Removes account \a index and its wallet entries. Returns how many
    /// accounts are left, or -1 when the index was unknown.
    int remove(int index);
    /// Appends \a account verbatim — the archive importer's way in, which
    /// writes a local account with nothing but a name and a storage key.
    /// Returns its index.
    int append(const QVariantMap &account);
    /// Index of the account filed under \a cacheKey, or -1. Imported archives
    /// are the only accounts that carry one, and it is what names them.
    int indexOfCacheKey(const QString &cacheKey) const;
    /// Takes every account carrying \a cacheKey back out — an import that
    /// found no mail, undoing the shell it created. Returns true if any went.
    bool removeByCacheKey(const QString &cacheKey);
    /// Reorders the array. Returns false when the move is a no-op or invalid.
    bool move(int from, int to);

    // --- secrets ---
    QString password() const { return m_password; }
    void setPassword(const QString &password) { m_password = password; }
    QString refreshToken() const { return m_refreshToken; }
    void setRefreshToken(const QString &token) { m_refreshToken = token; }
    QString accessToken() const { return m_accessToken; }
    QDateTime accessTokenExpiry() const { return m_accessTokenExpiry; }
    void setAccessToken(const QString &token, const QDateTime &expiry)
    {
        m_accessToken = token;
        m_accessTokenExpiry = expiry;
    }
    /// Tokens never survive an account switch.
    void clearTokens();
    bool secretReady() const { return m_secretReady; }
    /// Declares the secret settled without a wallet read — a local archive
    /// owns none, and a password handed in for this session is already here.
    void markSecretReady() { m_secretReady = true; }
    /// Takes \a password as this session's secret outright, abandoning any
    /// wallet read still in flight for the account being left. An empty
    /// password is the local-archive case: there is nothing to look up.
    void setSessionSecret(const QString &password);
    /// Reads \a cfg's secret out of the wallet (password, or refresh token for
    /// an OAuth account), then emits secretsReady(). Any read still in flight
    /// for a previous account is abandoned rather than applied.
    void readSecret(const AccountConfig &cfg);
    /// Stores the current password under \a cfg's wallet key, and clears the
    /// pre-wallet plaintext once the wallet definitely has it.
    void writeSecretToWallet(const AccountConfig &cfg);
    /// Takes the pre-wallet base64 password out of the config file, if one is
    /// still there. Returns false when there was none.
    bool takeLegacySecret(QString *password);

    static QString walletKeyFor(const QString &user, const QString &host);
    static QString oauthWalletKeyFor(const QString &user, const QString &host);

Q_SIGNALS:
    /// A wallet read finished (successfully or not) for the account that is
    /// still the current one.
    void secretsReady();
    void errorOccurred(const QString &message);

private:
    int m_current = 0;
    QString m_password;
    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_accessTokenExpiry;
    bool m_secretReady = false; ///< wallet lookup finished (or not needed)
    int m_walletGen = 0; ///< invalidates in-flight wallet reads on account switch
};
