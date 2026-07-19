#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "foldermodel.h"
#include "mailstore.h"
#include "messagelistmodel.h"

namespace KIMAP
{
class Session;
class IdleJob;
class LoginJob;
}
namespace KMime
{
class Content;
class Message;
}
class OAuthHelper;
class ViewerSchemeHandler;

/**
 * Central IMAP controller exposed to QML as the "Mail" singleton.
 *
 * All jobs are async KJobs on the event loop; no extra threads are involved.
 * KIMAP runs jobs on one connection strictly in order, so interactive work
 * (main session), IDLE push, and background sync (header backfill + body
 * prefetch) each get their own connection — user actions never queue behind
 * a long background transfer.
 */
class MailClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY accountChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(FolderModel *folderModel READ folderModel CONSTANT)
    Q_PROPERTY(MessageListModel *messageModel READ messageModel CONSTANT)
    /// Attachments of the last fetched message: [{name, sizeText}, …]
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    /// Per-message opt-in for remote images/CSS/fonts; resets on every message.
    Q_PROPERTY(bool remoteContentAllowed READ remoteContentAllowed
                   WRITE setRemoteContentAllowed NOTIFY remoteContentAllowedChanged)
    /// Instant plain-text stand-in shown while the HTML view renders.
    Q_PROPERTY(QString textPreview READ textPreview NOTIFY textPreviewChanged)

    // Account fields for prefilling the settings sheet
    Q_PROPERTY(QString accountHost READ accountHost NOTIFY accountChanged)
    Q_PROPERTY(int accountPort READ accountPort NOTIFY accountChanged)
    Q_PROPERTY(QString accountUser READ accountUser NOTIFY accountChanged)
    Q_PROPERTY(int accountSecurity READ accountSecurity NOTIFY accountChanged)
    Q_PROPERTY(QString smtpHost READ smtpHost NOTIFY accountChanged)
    Q_PROPERTY(int smtpPort READ smtpPort NOTIFY accountChanged)
    Q_PROPERTY(int smtpSecurity READ smtpSecurity NOTIFY accountChanged)

    // All configured accounts (display names) and which one is active
    Q_PROPERTY(QStringList accountNames READ accountNames NOTIFY accountsChanged)
    Q_PROPERTY(int currentAccount READ currentAccount NOTIFY accountsChanged)
    /// Bumped whenever a cached folder tree changes (collapse toggle) — QML
    /// bindings reference it so cachedFolderList() gets re-evaluated.
    Q_PROPERTY(int cachedFolderRevision READ cachedFolderRevision NOTIFY cachedFoldersChanged)

    /// Poll interval for the open folder in minutes, used only while IMAP
    /// IDLE push is not active. 0 disables polling. Persisted.
    Q_PROPERTY(int refreshMinutes READ refreshMinutes WRITE setRefreshMinutes
                   NOTIFY refreshMinutesChanged)

    /// Qt date pattern (e.g. "dd/MM/yyyy") used for message dates in the list
    /// and the viewer; today's messages show only the time. Persisted.
    Q_PROPERTY(QString dateFormat READ dateFormat WRITE setDateFormat
                   NOTIFY dateFormatChanged)

public:
    // Keep in sync with the combo box in AccountSheet.qml
    enum Security { SslTls = 0, StartTls = 1, None = 2 };
    Q_ENUM(Security)

    explicit MailClient(QObject *parent = nullptr);

    /// The scheme handler that serves message bodies to the viewer.
    void setViewerHandler(ViewerSchemeHandler *handler) { m_viewerHandler = handler; }

    bool hasAccount() const;
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    QString statusText() const { return m_statusText; }
    FolderModel *folderModel() { return &m_folderModel; }
    MessageListModel *messageModel() { return &m_messageModel; }

    QString accountHost() const { return m_host; }
    int accountPort() const { return m_port; }
    QString accountUser() const { return m_user; }
    int accountSecurity() const { return m_security; }

    QString smtpHost() const { return m_smtpHost; }
    int smtpPort() const { return m_smtpPort; }
    int smtpSecurity() const { return m_smtpSecurity; }

    QStringList accountNames() const;
    int currentAccount() const { return m_currentAccount; }
    int cachedFolderRevision() const { return m_cachedFolderRevision; }
    int refreshMinutes() const { return m_refreshMinutes; }
    void setRefreshMinutes(int minutes);
    QString dateFormat() const { return m_dateFormat; }
    void setDateFormat(const QString &format);
    /// Cached folder tree of account \a index for the sidebar, as a list of
    /// {name, mailBox, level} maps — available even while the account is not
    /// the connected one.
    Q_INVOKABLE QVariantList cachedFolderList(int index);
    /// Collapses/expands a folder in the cached tree of account \a index
    /// (persisted; the sidebar re-reads cachedFolderList on cachedFoldersChanged).
    Q_INVOKABLE void toggleCachedCollapsed(int index, const QString &mailBox);
    /// Opens a folder that may belong to another account: switches the
    /// connection there first when needed, then opens the folder.
    Q_INVOKABLE void openFolderInAccount(int index, const QString &mailBox);
    /// Config fields of account \a index as {host, port, security, user,
    /// smtpHost, smtpPort, smtpSecurity, authType, clientId, clientSecret};
    /// empty map for an unknown index.
    Q_INVOKABLE QVariantMap accountDetails(int index) const;
    /// Creates (index -1 or out of range) or updates an account from the same
    /// map shape accountDetails() returns (plus "password"/"savePassword"),
    /// then makes it the active one. An empty password keeps the stored one.
    Q_INVOKABLE void saveAccountDetails(int index, const QVariantMap &details);
    Q_INVOKABLE void removeAccount(int index);
    /// Disconnects, loads account \a index and reconnects.
    Q_INVOKABLE void switchAccount(int index);
    /// Builds a MIME message and sends it via SMTP. attachments are local file URLs.
    Q_INVOKABLE void sendMail(const QString &to, const QString &cc, const QString &subject,
                              const QString &html, const QList<QUrl> &attachments);
    /// Compose prefill for replying to the currently shown message:
    /// {to, cc, subject, body} — body is HTML with the original quoted.
    /// cc is filled only for \a replyAll. Empty map when nothing is shown.
    Q_INVOKABLE QVariantMap replyData(bool replyAll);
    /// Compose prefill for forwarding the currently shown message:
    /// {to, cc, subject, body} — to/cc empty, subject "Fwd:"-prefixed, body
    /// HTML with the original quoted. Empty map when nothing is shown.
    /// The original's attachments are NOT carried over (compose attaches
    /// local files only).
    Q_INVOKABLE QVariantMap forwardData();
    /// Initial body for a brand-new message: a blank line for the cursor
    /// followed by the account's signature (empty when no signature is set).
    Q_INVOKABLE QString newMessageBody() const;
    /// Reads a local HTML file (signature import). Returns its content, or
    /// an empty string on failure (an error notification is emitted then).
    Q_INVOKABLE QString loadHtmlFile(const QUrl &fileUrl);
    /// Known recipient addresses (previously sent to) matching \a prefix,
    /// best-ranked first — compose field autocompletion.
    Q_INVOKABLE QStringList recipientSuggestions(const QString &prefix);
    Q_INVOKABLE void connectAccount();
    Q_INVOKABLE void openFolder(const QString &mailBox);
    Q_INVOKABLE void fetchMessage(int row);
    /// Quietly fetch a message's body into the cache (hover / read-ahead).
    /// No viewer or status changes; skipped when cached or offline.
    Q_INVOKABLE void prefetchMessage(int row);
    /// Fetches the next (older) window of headers; no-op if everything is loaded.
    Q_INVOKABLE void loadMoreMessages();

    /// True when the currently open folder is the trash folder.
    Q_INVOKABLE bool isTrashFolder() const;
    /// Deletes the given model rows: moves them to Trash, or — when the open
    /// folder IS the trash — flags \Deleted and expunges (permanent).
    /// The QML side is responsible for confirming the permanent case.
    Q_INVOKABLE void deleteMessages(const QVariantList &rows);

    /// Server-side IMAP SEARCH in the selected folder.
    /// field: 0 = whole message, 1 = subject, 2 = from, 3 = body.
    /// A query wrapped in slashes (/pattern/) instead regex-filters the loaded list.
    Q_INVOKABLE void searchMessages(const QString &query, int field);
    /// Leaves search mode and reloads the folder.
    Q_INVOKABLE void clearSearch();

    // --- Viewer debug helpers (operate on the last fetched message) ---
    /// Rendered HTML view; falls back to the text part when there is no HTML.
    Q_INVOKABLE QString htmlViewUrl();
    /// Plain-text part of the message ("discard HTML").
    Q_INVOKABLE QString textViewUrl();
    /// Raw HTML source, escaped, monospace — our "view page source".
    Q_INVOKABLE QString sourceViewUrl();

    QVariantList attachments() const { return m_attachments; }
    bool remoteContentAllowed() const { return m_remoteContentAllowed; }
    void setRemoteContentAllowed(bool allow);
    QString textPreview() const { return m_textPreview; }
    /// Writes attachment \a index of the current message to \a fileUrl.
    Q_INVOKABLE void saveAttachment(int index, const QUrl &fileUrl);
    /// True when the attachment could execute code if opened (.sh, .desktop,
    /// AppImage, .exe, …) — the UI shows a confirmation first.
    Q_INVOKABLE bool attachmentRisky(int index) const;
    /// Opens attachment \a index with the system handler (via a temp copy).
    Q_INVOKABLE void openAttachment(int index);
    /// Saves attachment \a index into ~/Downloads under its own filename,
    /// deduplicating ("name (1).pdf") instead of overwriting.
    Q_INVOKABLE void saveAttachmentToDownloads(int index);
    /// Opens a link from a message in the system browser / mail handler.
    Q_INVOKABLE void openExternalUrl(const QUrl &url);

Q_SIGNALS:
    void accountChanged();
    void connectedChanged();
    void busyChanged();
    void statusTextChanged();
    /// Fired when a full message body has been fetched and parsed.
    /// bodyUrl is a mailo:// URL the viewer should load.
    void messageLoaded(const QString &subject, const QString &from,
                       const QString &to, const QString &cc,
                       const QString &date, const QString &bodyUrl,
                       const QString &authInfo);
    void errorOccurred(const QString &message);
    void mailSent();
    /// Folder contents were refreshed from the server (initial or re-open).
    void folderRefreshed();
    void attachmentsChanged();
    void remoteContentAllowedChanged();
    void textPreviewChanged();
    void accountsChanged();
    void cachedFoldersChanged();
    void refreshMinutesChanged();
    void dateFormatChanged();

private:
    QString accountKey() const { return m_user + QLatin1Char('@') + m_host; }
    /// Fills the folder model from the disk cache (instant sidebar).
    void loadCachedFolderModel();
    /// Records a body-derived attachment kind (e.g. calendar invite) in the
    /// cache and the visible list.
    void refineAttachKind(const QString &folder, qint64 uid, KMime::Message *msg);
    void loadAccount();
    void loadAccountFields();
    void readWalletPassword();
    void switchAccountInternal(int index, const QString &sessionPassword);
    QString walletKey() const;
    void writeSecretToWallet();
    void setBusy(bool busy);
    void setStatus(const QString &text);
    void listFolders();
    void fetchHeaders(qint64 fromSeq, qint64 toSeq, bool append);
    /// The actual header FETCH on \a session. \a background jobs never touch
    /// busy state, and any result for a folder the user has left goes to the
    /// cache only — never into the visible list.
    void fetchHeadersOn(KIMAP::Session *session, const QString &folder,
                        qint64 fromSeq, qint64 toSeq, bool append, bool background);
    /// Opens the dedicated background-sync connection (best-effort).
    void startSyncSession();
    /// Runs \a fn with the sync session once \a folder is selected on it;
    /// falls back to the main session when no sync connection exists, and
    /// passes nullptr when neither can serve the folder.
    void withSyncSession(const QString &folder,
                         const std::function<void(KIMAP::Session *)> &fn);
    /// Fetches everything the server has above the cached block (by UID),
    /// then resumes the backfill cursor below the block — old mail never
    /// changes, so the cached middle needs no refetch.
    void fetchNewerThanCache(qint64 maxCachedUid, int cachedCount);
    /// Fetches the next older header window from the server (backfill step).
    void fetchOlderFromServer();
    /// Idle-time body caching: queues the next few headers that have no
    /// cached body yet. Runs only after the header backfill has finished,
    /// so a fresh account always shows the full list first.
    void backfillBodies();
    /// Remembers the oldest (date, uid) shown from the disk cache, so
    /// loadMoreMessages() can page the next cached chunk in from there.
    void updatePageAnchor(const QList<MessageListModel::Header> &page);
    /// Fetches the given uids as search results; when \a localMergeKeyword is
    /// set, local partial-match hits are merged into the result list.
    void fetchHeadersByUids(const QList<qint64> &uids,
                            const QString &localMergeKeyword = {});
    void localKeywordFilter(const QString &keyword, const QString &reason);
    void appendToSentFolder(const QByteArray &rawMessage);
    void collectInlineParts(KMime::Content *root);
    void collectAttachments(KMime::Content *root);
    QString attachmentName(int index) const;
    bool writeAttachment(int index, const QString &path);
    void presentMessage(const std::shared_ptr<KMime::Message> &message);
    /// Records the To/Cc addresses of a message from the Sent folder in the
    /// recipient-autocompletion store.
    void harvestRecipients(const KMime::Message *msg);
    /// The account's own sending address (used as From, and excluded from
    /// reply-all recipient lists).
    QString ownAddress() const;
    /// The account signature as an HTML fragment ready to splice into a
    /// compose body; empty when no signature is set.
    QString signatureBlock() const;
    QString trashFolderName() const;
    void purgeDeleted(const QList<qint64> &uids);
    void configureLogin(KIMAP::LoginJob *login) const;
    QString oauthWalletKey() const;
    /// Obtains a fresh access token (refresh grant or browser sign-in), then
    /// re-enters connectAccount().
    void acquireTokenAndConnect();
    void startIdle();
    void stopIdle();
    /// Merges any new server messages into the open folder without clearing it.
    void refreshCurrentFolder();
    /// Arms the idle-time fetch of the next older header window.
    void scheduleBackfill();
    /// Updates viewer remote-content policy without persisting a preference.
    void applyRemoteContentAllowed(bool allow);
    void processPrefetchQueue();
    void teardownSession();
    /// One tiny batch of search-index repair (bodies queued in fts_pending):
    /// parses the raw message and writes its text into the FTS index. Timer-
    /// driven so the GUI thread never does more than a few ms at a time.
    void reindexPendingBodies();

    QString m_host;
    int m_port = 993;
    int m_security = SslTls;
    QString m_user;
    QString m_password;
    QString m_smtpHost;
    int m_smtpPort = 587;
    int m_smtpSecurity = 1; // Session::EncryptionMode-ish: 0 TLS, 1 STARTTLS, 2 none
    int m_authType = 0;     // 0 password, 1 Gmail OAuth2, 2 Microsoft OAuth2
    QString m_clientId;
    QString m_clientSecret;
    QString m_signature; ///< per-account signature (HTML, may be a full doc)
    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_accessTokenExpiry;
    OAuthHelper *m_oauth = nullptr;
    bool m_secretReady = false;      ///< wallet lookup finished (or not needed)
    bool m_connectWhenReady = false; ///< connect was requested before that
    int m_currentAccount = 0;
    int m_cachedFolderRevision = 0; ///< see cachedFolderRevision property
    int m_walletGen = 0; ///< invalidates in-flight wallet reads on account switch

    ViewerSchemeHandler *m_viewerHandler = nullptr;
    QPointer<KIMAP::Session> m_session;
    QPointer<KIMAP::Session> m_idleSession; ///< dedicated connection for IMAP IDLE push
    QPointer<KIMAP::IdleJob> m_idleJob;
    QPointer<KIMAP::Session> m_syncSession; ///< dedicated connection for background sync
    bool m_syncReady = false; ///< the sync connection is logged in
    QString m_syncFolder;     ///< mailbox currently selected on the sync connection
    QString m_selectedFolder;
    QString m_pendingFolder; ///< folder to reopen after (re)connect
    QString m_sentFolder;    ///< where sent mail gets APPENDed
    QTimer m_keepAlive;
    QTimer m_pollTimer;      ///< IDLE-less fallback refresh of the open folder
    int m_refreshMinutes = 5;
    QString m_dateFormat;    ///< Qt date pattern for list/viewer dates
    qint64 m_oldestFetchedSeq = 0; ///< lowest sequence number fetched so far
    qint64 m_folderMessageCount = 0; ///< total messages in the open folder
    QTimer m_reindexTimer;   ///< drip-feed repair of the body search index
    QTimer m_backfillTimer;  ///< idle-time fetch of older header windows
    bool m_backfill = false; ///< the running header fetch is a backfill one
    bool m_headerFetch = false; ///< a header FETCH is in flight (any session)
    bool m_bodyBackfill = false; ///< the idle body-caching phase is running
    bool m_searchActive = false; ///< showing search results, not the folder
    qint64 m_pageDate = 0;   ///< disk-cache paging anchor: oldest shown date
    qint64 m_pageUid = 0;    ///< …and its uid (keyset pagination tiebreaker)

    QString m_currentHtmlBody; ///< raw HTML part of the last fetched message
    QString m_currentTextBody; ///< plain-text part of the last fetched message
    QByteArray m_currentRaw;   ///< complete RFC-822 source of the last message
    std::shared_ptr<KMime::Message> m_currentMessage; ///< keeps attachment parts alive
    QList<KMime::Content *> m_attachmentParts; ///< owned by m_currentMessage
    QVariantList m_attachments;
    bool m_remoteContentAllowed = false;
    QString m_currentSenderAddress; ///< addr-spec of the shown message's sender
    QString m_textPreview;
    QList<qint64> m_prefetchQueue; ///< uids waiting for a background body fetch
    bool m_prefetching = false;
    bool m_connected = false;
    bool m_busy = false;
    QString m_statusText;

    FolderModel m_folderModel;
    MessageListModel m_messageModel;
    MailStore m_store;
};
