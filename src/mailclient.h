// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QAtomicInt>
#include <QMutex>
#include <QWaitCondition>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "dkimverifier.h"
#include "foldermodel.h"
#include "mailstore.h"
#include "messagecontext.h"
#include "messagelistmodel.h"

class QThread;

class QThread;

namespace KIMAP
{
class Session;
class IdleJob;
class LoginJob;
class ImapSet;
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
    /// True while a cache vacuum is running — the UI disables the button and
    /// shows a spinner, since the database is locked for writes meanwhile.
    Q_PROPERTY(bool reclaiming READ reclaiming NOTIFY reclaimingChanged)
    /// True while the search index is being rebuilt to fold diacritics. The
    /// window refuses to close during it: the swap at the end is what makes
    /// the work count, and abandoning it means starting the pass again.
    Q_PROPERTY(bool indexRebuilding READ indexRebuilding NOTIFY indexRebuildChanged)
    /// True from the moment a search starts until the last result is in. The
    /// list shows it directly — an empty list during a slow search is
    /// indistinguishable from "nothing found" unless the view says otherwise.
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    /// Hits delivered so far by the search in flight.
    Q_PROPERTY(int searchFound READ searchFound NOTIFY searchingChanged)
    /// How far that has got, 0-100, for the message shown on a close attempt.
    Q_PROPERTY(int indexRebuildPercent READ indexRebuildPercent NOTIFY indexRebuildChanged)
    /// ABOUT.md compiled into the binary (Settings → About). CONSTANT — baked
    /// in at build time, never changes at runtime.
    Q_PROPERTY(QString aboutText READ aboutText CONSTANT)
    Q_PROPERTY(FolderModel *folderModel READ folderModel CONSTANT)
    Q_PROPERTY(MessageListModel *messageModel READ messageModel CONSTANT)
    /// The reading pane's message context — the state MessageViewer binds to.
    /// Detached message windows get their own context via messageWindowReady.
    Q_PROPERTY(MessageContext *readingContext READ readingContext CONSTANT)
    /// Attachments of the last fetched message: [{name, sizeText}, …]
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    /// Per-message opt-in for remote images/CSS/fonts; resets on every message.
    Q_PROPERTY(bool remoteContentAllowed READ remoteContentAllowed
                   WRITE setRemoteContentAllowed NOTIFY remoteContentAllowedChanged)
    /// Instant plain-text stand-in shown while the HTML view renders.
    Q_PROPERTY(QString textPreview READ textPreview NOTIFY textPreviewChanged)
    /// True while the shown message comes from a junk/spam folder: the viewer
    /// then defaults to plain text and renders HTML only on explicit request.
    Q_PROPERTY(bool junkTextOnly READ junkTextOnly NOTIFY junkTextOnlyChanged)

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
    /// Largest message body to keep in the offline cache, in MB (0 = no
    /// limit). Bigger ones are still opened on demand, just never stored.
    Q_PROPERTY(int maxBodyMB READ maxBodyMB WRITE setMaxBodyMB NOTIFY maxBodyMBChanged)
    /// Writes a running trace of folder/account/sync activity to the console.
    /// Off by default; takes effect immediately. Persisted.
    Q_PROPERTY(bool debugLogging READ debugLogging WRITE setDebugLogging
                   NOTIFY debugLoggingChanged)
    /// The folder actually open right now. The sidebar follows this rather
    /// than deciding for itself which row is open.
    Q_PROPERTY(QString selectedFolder READ selectedFolder NOTIFY selectedFolderChanged)

    /// Qt date pattern (e.g. "dd/MM/yyyy") used for message dates in the list
    /// and the viewer; today's messages show only the time. Persisted.
    Q_PROPERTY(QString dateFormat READ dateFormat WRITE setDateFormat
                   NOTIFY dateFormatChanged)

public:
    // Keep in sync with the combo box in AccountSheet.qml
    enum Security { SslTls = 0, StartTls = 1, None = 2 };
    Q_ENUM(Security)

    explicit MailClient(QObject *parent = nullptr);
    /// Stops and joins the cache workers — both hold a raw `this` and a SQLite
    /// connection, neither of which may outlive the client.
    ~MailClient() override;

    /// The scheme handler that serves message bodies to the viewer.
    void setViewerHandler(ViewerSchemeHandler *handler) { m_viewerHandler = handler; }

    bool hasAccount() const;
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    QString statusText() const { return m_statusText; }
    QString aboutText() const;
    FolderModel *folderModel() { return &m_folderModel; }
    MessageListModel *messageModel() { return &m_messageModel; }
    MessageContext *readingContext() { return m_reading; }

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
    int maxBodyMB() const { return m_maxBodyMB; }
    void setMaxBodyMB(int mb);
    QString selectedFolder() const { return m_selectedFolder; }
    bool debugLogging() const { return m_debugLogging; }
    void setDebugLogging(bool on);
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
    Q_INVOKABLE void sendMail(const QString &to, const QString &cc, const QString &bcc,
                              const QString &subject, const QString &html,
                              const QList<QUrl> &attachments);
    /// APPENDs the same message to the Drafts folder instead of sending it.
    /// Unlike sendMail this accepts an unfinished message — no recipient, or
    /// an address still being typed — because that is the state a draft is
    /// saved from. Emits draftSaved() or sendFailed().
    /// \a replacesUid is the draft being re-saved (-1 for a new one); it is
    /// removed only after the server has accepted the replacement.
    Q_INVOKABLE void saveDraft(const QString &to, const QString &cc, const QString &bcc,
                               const QString &subject, const QString &html,
                               const QList<QUrl> &attachments, qint64 replacesUid = -1);
    /// Whether a Drafts folder is known, so the UI can hide the action when
    /// there is nowhere to put one.
    Q_PROPERTY(bool hasDraftsFolder READ hasDraftsFolder NOTIFY draftsFolderChanged)
    bool hasDraftsFolder() const { return !m_draftsFolder.isEmpty(); }
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
    /// Compose prefill for editing the currently shown message as a draft:
    /// {to, cc, bcc, subject, body, uid} — the message verbatim, not quoted.
    /// uid is the draft's own uid so the old copy can be removed on send.
    /// Empty map when nothing is shown.
    Q_INVOKABLE QVariantMap draftData();
    /// True while the open folder is this account's Drafts folder — clicking a
    /// message there reopens it in the composer instead of the reader.
    Q_PROPERTY(bool viewingDrafts READ viewingDrafts NOTIFY selectedFolderChanged)
    bool viewingDrafts() const
    {
        return !m_draftsFolder.isEmpty() && m_selectedFolder == m_draftsFolder;
    }
    /// Deletes a draft by uid — the superseded copy, once its replacement has
    /// been sent or re-saved.
    Q_INVOKABLE void discardDraft(qint64 uid);
    /// Copy the given text to the system clipboard (used to grab the full
    /// status breadcrumb trail on right-click).
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
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
    /// Opens the message at \a row in its own top-level window: fetches it
    /// (cache or server) and emits messageWindowReady() with a fresh
    /// MessageContext once it is presentable.
    Q_INVOKABLE void openMessageInWindow(int row);
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

    /// Moves the given model rows to the account's junk/spam folder.
    /// No-op when the open folder already is the junk folder.
    Q_INVOKABLE void markAsJunk(const QVariantList &rows);

    /// Moves the given model rows into \a targetFolder — the sidebar's
    /// drag-and-drop target. No-op when it is the folder they are already in.
    Q_INVOKABLE void moveMessagesTo(const QVariantList &rows, const QString &targetFolder);

    /// True when \a mailBox may be dropped onto \a newParent ("" = top level):
    /// a real, different, non-protected folder that is not inside \a mailBox
    /// itself, and whose resulting path is still free. The sidebar asks this
    /// per hovered row, so it only outlines targets a drop would work on.
    Q_INVOKABLE bool canMoveFolder(const QString &mailBox, const QString &newParent) const;
    /// Reparents \a mailBox (with its whole subtree) under \a newParent via
    /// IMAP RENAME; "" moves it to the top level. The cached mail moves with
    /// it — see MailStore::renameFolderOn.
    Q_INVOKABLE void moveFolder(const QString &mailBox, const QString &newParent);

    /// True for the folders that must not be moved or deleted: INBOX and the
    /// account's special-use mailboxes (sent, trash, junk).
    Q_INVOKABLE bool folderProtected(const QString &mailBox) const;
    /// True when deleting \a mailBox removes it from the server for good —
    /// it already lives in the trash (or the account has no trash folder).
    /// The confirmation dialog is worded from this.
    Q_INVOKABLE bool folderDeleteIsPermanent(const QString &mailBox) const;
    /// Deletes \a mailBox: moves it into the trash, or — when it is already
    /// there — removes it and its subfolders from the server. The QML side is
    /// responsible for confirming either case.
    Q_INVOKABLE void deleteFolder(const QString &mailBox);

    /// Local-only color-scale mark (1..5) for the given model rows; a row
    /// already carrying that color is cleared instead (toggle). 0 clears —
    /// a scale slot left without a color acts as the "clear mark" shortcut.
    Q_INVOKABLE void markMessageColor(const QVariantList &rows, int color);

    /// Quick filter: show only messages marked with this color (0 = off).
    /// Queries the disk cache so marks outside the loaded page appear too.
    Q_INVOKABLE void filterByColor(int color);

    /// Server-side IMAP SEARCH in the selected folder.
    /// field: 0 = from + subject only, 1 = everything (body, cc, all headers).
    /// A query wrapped in slashes (/pattern/) instead regex-filters the loaded list.
    Q_INVOKABLE void searchMessages(const QString &query, int field);
    /// Leaves search mode and reloads the folder.
    Q_INVOKABLE void clearSearch();

    // --- Viewer debug helpers (operate on the last fetched message) ---
    /// Rendered HTML view; falls back to the text part when there is no HTML.
    Q_INVOKABLE QString htmlViewUrl();
    /// Plain-text part of the message ("discard HTML").
    Q_INVOKABLE QString textViewUrl();
    /// The complete raw RFC-822 message (headers + MIME structure + parts),
    /// escaped, monospace — our "view message source".
    Q_INVOKABLE QString sourceViewUrl();

    // The message-state properties delegate to the reading pane's context —
    // kept on Mail so existing callers keep working; per-window state lives
    // on each window's own MessageContext.
    QVariantList attachments() const { return m_reading->attachments(); }
    bool remoteContentAllowed() const { return m_reading->remoteContentAllowed(); }
    void setRemoteContentAllowed(bool allow);
    QString textPreview() const { return m_textPreview; }
    bool junkTextOnly() const { return m_reading->junkTextOnly(); }
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

    // --- Cache maintenance (Settings → Storage) ---
    /// Human-readable cache size, e.g. "13.4 GB (6.2 GB reclaimable)".
    Q_INVOKABLE QString cacheSizeText();
    /// Rebuilds the cache file on a worker thread to hand free pages back to
    /// the filesystem. Deleting messages only marks pages reusable — the file
    /// itself never shrinks without this. Sync is paused for the duration.
    Q_INVOKABLE void reclaimDiskSpace();
    /// Whether a rebuild would actually hand anything back. False right after
    /// one has run, so the UI can stop offering a multi-minute no-op.
    Q_INVOKABLE bool reclaimWorthwhile();
    bool reclaiming() const { return m_reclaiming; }
    bool indexRebuilding() const { return m_indexRebuilding; }
    bool searching() const { return m_searching; }
    int searchFound() const { return m_searchFound; }
    int indexRebuildPercent() const { return m_indexPercent; }
    /// Asks to quit as soon as the rebuild finishes; the window is closed for
    /// the user rather than leaving them to try again.
    Q_INVOKABLE void quitWhenIndexRebuildDone() { m_quitAfterIndex = true; }

Q_SIGNALS:
    void reclaimingChanged();
    void indexRebuildChanged();
    void searchingChanged();
    /// The rebuild finished and a close was pending — QML closes the window,
    /// so it still saves its geometry on the way out.
    void closeRequested();
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
    /// A double-clicked message is ready to show in its own window. The
    /// context is parented to this client; the window calls release() on it
    /// when it closes.
    void messageWindowReady(MessageContext *context);
    void mailSent();
    void draftSaved();
    void draftsFolderChanged();
    /// Sending failed — carries the full (unshortened) server error. The
    /// compose window stays open and shows this in a dismissible dialog; it is
    /// deliberately NOT routed through the status log.
    void sendFailed(const QString &error);
    /// Folder contents were refreshed from the server (initial or re-open).
    void folderRefreshed();
    void attachmentsChanged();
    void remoteContentAllowedChanged();
    void textPreviewChanged();
    void junkTextOnlyChanged();
    void accountsChanged();
    void cachedFoldersChanged();
    void refreshMinutesChanged();
    void maxBodyMBChanged();
    void debugLoggingChanged();
    void selectedFolderChanged();
    void dateFormatChanged();

private:
    friend class MessageContext; // thin QML front for the *For methods below

    QString accountKey() const { return m_user + QLatin1Char('@') + m_host; }
    /// Fills the folder model from the disk cache (instant sidebar).
    void loadCachedFolderModel();
    /// Records a body-derived attachment kind (e.g. calendar invite) in the
    /// cache and the visible list.
    void refineAttachKind(const QString &folder, qint64 uid, KMime::Message *msg);
    /// Marks a message read everywhere: visible list, disk cache, and (when
    /// online) the server via STORE \Seen — so the state survives restarts.
    void markMessageRead(int row);
    void loadAccount();
    void loadAccountFields();
    void readWalletPassword();
    /// Switches to account \a index. \a targetFolder is the folder to land
    /// on — its cached contents are shown immediately, and it is what the
    /// connection opens once the folder list arrives. Empty means INBOX.
    void switchAccountInternal(int index, const QString &sessionPassword,
                               const QString &targetFolder = {});
    QString walletKey() const;
    void writeSecretToWallet();
    void setBusy(bool busy);
    void setStatus(const QString &text);
    /// Composed background-sync status for the open folder: the header-sync
    /// progress ("N of M synced") and the body-caching progress ("caching K
    /// bodies") shown together, so the two phases don't overwrite each other's
    /// numbers. Empty when nothing is syncing. \a folder must be the open one.
    QString openFolderSyncStatus(const QString &folder);
    /// Grow the background-sync backoff after server pushback (a throttling
    /// NO/BAD, or a dropped connection) and re-arm the backfill after that
    /// pause. Doubling, capped at 60 s.
    void backoffBackfill();
    /// Clear the throttle backoff/attempt state — called when a fetch succeeds
    /// and on (re)connect or folder change, so a healthy server resumes at
    /// full pace.
    void resetBackfillBackoff();
    /// Drop one background body-fetch connection and stop growing the pool,
    /// in response to a [TOO-MANY-SIMULTANEOUS-CONNECTIONS] refusal.
    void shrinkBodyPool();
    void listFolders();
    /// The server's hierarchy delimiter, as reported by LIST. Falls back to
    /// guessing from the known paths before the first listing has arrived.
    QChar folderSeparator() const;
    /// Last path component of a mailbox ("INBOX/a/b" -> "b").
    QString folderLeaf(const QString &mailBox) const;
    /// Everything above the last component ("INBOX/a/b" -> "INBOX/a"; empty
    /// for a top-level folder).
    QString folderParent(const QString &mailBox) const;
    /// \a leaf placed under \a parent ("" = top level), with " (2)", " (3)" …
    /// appended until the path is one no mailbox already uses.
    QString freeChildPath(const QString &parent, const QString &leaf) const;
    /// \a mailBox plus every folder below it, deepest first — the order a
    /// server will accept DELETEs in.
    QStringList folderSubtree(const QString &mailBox) const;
    /// RENAMEs \a from to \a to, then re-keys the cache, follows the open
    /// folder to its new path and re-lists the tree. \a doneStatus is the
    /// breadcrumb shown on success.
    void renameFolderOnServer(const QString &from, const QString &to,
                              const QString &doneStatus);
    /// Moves the cached mail of a renamed subtree onto its new paths, on a
    /// worker thread (it rewrites body blobs).
    void renameCachedFolder(const QString &from, const QString &to);
    /// Drops the cached mail of folders deleted from the server, on the same
    /// worker (chunked, so the GUI thread never waits for a write lock).
    void purgeCachedFolders(const QStringList &folders);
    /// Joins the folder-maintenance worker, if one is running.
    void stopFolderOps();
    /// True for a mailbox that duplicates every other one (Gmail's All Mail).
    static bool isAllMailName(const QString &mailBox);
    /// Queues a fetched body for the writer thread, and starts it if needed.
    void queueBodyWrite(MailStore::BodyWrite &&write);
    /// Writer-thread loop: drains m_bodyWriteQueue into batched transactions.
    void runBodyWriter();
    /// Stops and joins the writer thread after flushing its queue. It restarts
    /// on the next queueBodyWrite().
    void stopBodyWriter();

    /// Cached missingBodyCount() for one folder — see the .cpp for why.
    int missingBodiesIn(const QString &folder);
    void noteBodyStored(const QString &folder);
    void invalidateMissingBodies();

    /// Moves attachments of already-cached messages into the file store, a
    /// chunk at a time on a worker thread. Resumes after a restart.
    void startAttachmentMigration();
    void stopAttachmentMigration();
    /// Cancels the index rebuild between slices and joins its thread.
    void stopIndexRebuild();

    /// Starts the background removal of the excluded archive's cached rows.
    void startAllMailPurge();
    /// Cancels it and waits for the worker to finish.
    void stopAllMailPurge();
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
    /// Idle-time body caching: queues \a folder's next few headers that have
    /// no cached body yet. Runs only after the header backfill has finished,
    /// so a fresh account always shows the full list first. Returns false
    /// when the folder has no missing bodies (nothing was queued).
    bool backfillBodies(const QString &folder);
    /// Once the open folder is fully synced, walks the account's remaining
    /// folders (headers, then bodies) so every mailbox gets cached.
    void continueFolderBackfill();
    /// Remembers the oldest (date, uid) shown from the disk cache, so
    /// loadMoreMessages() can page the next cached chunk in from there.
    void updatePageAnchor(const QList<MessageListModel::Header> &page);
    /// Fetches the given uids as search results; when \a localMergeKeyword is
    /// set, local partial-match hits are merged into the result list.
    void fetchHeadersByUids(const QList<qint64> &uids, const QString &localMergeKeyword = {},
                            bool headersOnly = true);
    /// Local index search, off the GUI thread and streamed: rows appear as the
    /// worker finds them instead of after the last one. \a append keeps what is
    /// already listed (server hits being topped up with local partial matches);
    /// otherwise the first batch replaces the list.
    /// \a headersOnly limits matching to sender and subject.
    void localKeywordFilter(const QString &keyword, const QString &reason, bool append = false,
                            bool headersOnly = true);
    /// Copies the search index into one that folds diacritics, in slices on a
    /// worker, resuming where a previous run stopped.
    void startIndexRebuild();
    /// Removes rows the finished search did not confirm.
    void pruneSearchResults();
    /// Stops caring about the search in flight, if any. The worker notices on
    /// its next batch and gives up.
    void abandonLocalSearch() { m_searchSeq.fetchAndAddOrdered(1); }
    void appendToSentFolder(const QByteArray &rawMessage);
    /// Builds the MIME message shared by sendMail() and saveDraft().
    /// \a strict rejects malformed or missing recipients (sending); otherwise
    /// unparseable addresses are kept verbatim so a half-typed draft survives.
    /// Returns null and emits sendFailed() on a fatal problem.
    std::shared_ptr<KMime::Message> composeMessage(
        const QString &to, const QString &cc, const QString &bcc, const QString &subject,
        const QString &html, const QList<QUrl> &attachments, bool strict,
        QStringList *toList = nullptr, QStringList *ccList = nullptr,
        QStringList *bccList = nullptr);
    // --- Per-context message presentation (reading pane + detached windows) ---
    void collectInlineParts(MessageContext *ctx, KMime::Content *root);
    void collectAttachments(MessageContext *ctx, KMime::Content *root);
    QString attachmentNameFor(const MessageContext *ctx, int index) const;
    bool writeAttachmentFor(const MessageContext *ctx, int index, const QString &path);
    /// Presents \a message in the reading pane's context.
    void presentMessage(const std::shared_ptr<KMime::Message> &message);
    /// A standalone copy of the reading context for a detached window. Shares
    /// the parsed KMime message; gets its own scheme-handler slot so the
    /// window keeps rendering after the reading pane moves on.
    MessageContext *detachReading();
    // Context-parameterised backends of the public message API; MessageContext
    // delegates here (friend both ways keeps the logic in one place).
    QVariantMap replyDataFor(MessageContext *ctx, bool replyAll);
    QVariantMap forwardDataFor(MessageContext *ctx);
    QString htmlViewUrlFor(MessageContext *ctx);
    QString textViewUrlFor(MessageContext *ctx);
    QString sourceViewUrlFor(MessageContext *ctx);
    bool attachmentRiskyFor(const MessageContext *ctx, int index) const;
    void openAttachmentFor(MessageContext *ctx, int index);
    void saveAttachmentToDownloadsFor(MessageContext *ctx, int index);
    void saveAttachmentFor(MessageContext *ctx, int index, const QUrl &fileUrl);
    /// Persists the per-sender remote-content choice (MessageContext toggle).
    void rememberRemoteContent(const QString &senderAddress, bool allow);
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
    QString junkFolderName() const;
    /// Junk/spam folders get hostile-content handling in the viewer.
    bool isJunkFolder(const QString &mailBox) const;
    /// Hands \a ctx's message to the DKIM worker thread. Called only when a
    /// message is actually opened — never during sync or prefetch, so the
    /// mailbox does not turn into a stream of DNS queries to the resolver.
    void startDkimVerification(MessageContext *ctx);
    /// Hands the current message to the worker without resetting the retry
    /// count — the retry path re-enters here.
    void submitDkimVerification(MessageContext *ctx);
    /// DNS was unreachable rather than authoritative, so the key may well
    /// exist. Backs off and tries again a few times before giving up.
    /// Returns false when the attempts are exhausted.
    bool scheduleDkimRetry(MessageContext *ctx);
    /// Applies a verdict that arrived from the worker thread.
    void applyDkimResult(quint64 requestId, const DkimResult &result);
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
    void scheduleBackfill(int delayMs = 4000);
    void processPrefetchQueue();
    /// Parses and caches one prefetched body (raw + search text + refined
    /// attachment kind; recipient harvesting for the Sent folder).
    void storeFetchedBody(const QString &folder, qint64 uid,
                          const std::shared_ptr<KMime::Message> &message);
    /// One extra IMAP connection of the parallel body-caching pool.
    struct BodyConn {
        QPointer<KIMAP::Session> session;
        QString folder;    ///< mailbox currently selected on it
        bool ready = false;
        bool busy = false; ///< a body batch is streaming on it
    };
    /// Opens the missing pool connections (best effort, once per connect).
    void ensureBodyPool();
    /// Takes the next same-folder batch off the prefetch queue and streams
    /// it on \a conn, selecting the folder there first when needed.
    void dispatchBodyBatch(const std::shared_ptr<BodyConn> &conn);
    /// The streaming multi-UID body FETCH itself; \a release frees the
    /// issuing connection and is called exactly once.
    void startBodyFetchJob(KIMAP::Session *session, const QString &folder,
                           const KIMAP::ImapSet &set,
                           const std::function<void()> &release);
    /// True while any connection (pool or fallback) streams a body batch.
    bool bodyFetchActive() const;
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
    bool m_htmlMail = true; ///< send multipart text+HTML; false = plain text only
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
    bool m_folderReadWrite = false; ///< current SELECT is read-write (not EXAMINE)
    QString m_pendingFolder; ///< folder to reopen after (re)connect
    QString m_sentFolder;    ///< where sent mail gets APPENDed
    QString m_draftsFolder;  ///< where "Save as draft" APPENDs
    /// Gmail's \All archive: excluded from the folder list and the backfill
    /// because it re-stores every message already held under INBOX and labels.
    QString m_allMailFolder;
    QChar m_folderSeparator; ///< hierarchy delimiter reported by LIST
    int m_missingBodies = -1; ///< -1 = stale, recompute on next use
    QString m_missingBodiesFolder;
    int m_maxBodyMB = 5; ///< bodies above this are not cached (0 = no limit)
    bool m_debugLogging = false;

    /// Bodies wait here for the writer thread; the mutex guards the queue and
    /// pairs with m_bodyWriteWake.
    QList<MailStore::BodyWrite> m_bodyWriteQueue;
    QMutex m_bodyWriteMutex;
    QWaitCondition m_bodyWriteWake;
    QThread *m_bodyWriterThread = nullptr;
    QAtomicInt m_bodyWriterStop;

    QThread *m_migrateThread = nullptr; ///< attachment externalisation of old mail
    QAtomicInt m_migrateCancel;
    QThread *m_reindexThread = nullptr; ///< off-thread body text extraction
    QThread *m_purgeThread = nullptr; ///< background removal of that archive
    QThread *m_folderOpThread = nullptr; ///< cache re-key / purge after a folder move
    QAtomicInt m_folderOpCancel;
    QThread *m_vacuumThread = nullptr;
    QAtomicInt m_purgeCancel;
    int m_purgedRows = 0;
    bool m_reclaiming = false; ///< a VACUUM is running on a worker thread
    QThread *m_indexThread = nullptr; ///< diacritics rebuild of the FTS index
    QAtomicInt m_indexCancel;
    /// Read by the body-writer thread, so it knows to queue what it indexes
    /// for repair after the swap.
    QAtomicInt m_indexRebuildActive;
    bool m_indexRebuilding = false;
    bool m_searching = false;   ///< a search is in flight (server or local)
    int m_searchFound = 0;      ///< hits delivered by it so far
    int m_indexPercent = 0;
    bool m_quitAfterIndex = false; ///< close was attempted mid-rebuild
    QTimer m_keepAlive;
    QTimer m_pollTimer;      ///< IDLE-less fallback refresh of the open folder
    int m_refreshMinutes = 5;
    QString m_dateFormat;    ///< Qt date pattern for list/viewer dates
    qint64 m_oldestFetchedSeq = 0; ///< lowest sequence number fetched so far
    qint64 m_folderMessageCount = 0; ///< total messages in the open folder
    QTimer m_reindexTimer;   ///< drip-feed repair of the body search index
    QTimer m_backfillTimer;  ///< idle-time fetch of older header windows
    bool m_backfill = false; ///< the running header fetch is a backfill one
    int m_backfillAttempt = 0; ///< consecutive throttle hits (0 while healthy)
    bool m_syncPaused = false; ///< backfill suspended after too many throttles
    QStringList m_folderBackfillQueue; ///< folders still to background-sync
    QString m_backfillFolder;   ///< non-open folder currently background-syncing
    qint64 m_backfillOldestSeq = 0; ///< its header cursor (0 = size unknown yet)
    bool m_folderBackfillPassDone = false; ///< all folders visited this connect
    bool m_headerFetch = false; ///< a header FETCH is in flight (any session)
    bool m_bodyBackfill = false; ///< the idle body-caching phase is running
    bool m_searchActive = false; ///< showing search results, not the folder
    /// Bumped for every local search started or abandoned. The worker carries
    /// the value it was started with and stops as soon as it no longer
    /// matches, so a user who keeps typing is never waiting on the query for
    /// the word they have already changed.
    QAtomicInteger<quint64> m_searchSeq;
    /// Uids the current search has delivered (GUI thread only). When the
    /// search completes, rows outside this set are pruned — the list morphs
    /// from the old query's results into the new one's instead of being
    /// cleared up front, which flashed blank on every keystroke.
    QSet<qint64> m_searchSeen;
    qint64 m_pageDate = 0;   ///< disk-cache paging anchor: oldest shown date
    qint64 m_pageUid = 0;    ///< …and its uid (keyset pagination tiebreaker)

    MessageContext *m_reading = nullptr; ///< the reading pane's message state

    /// DKIM verification runs off the GUI thread: it is a DNS round trip plus
    /// SHA-256 over the whole message and a public-key operation.
    QThread *m_dkimThread = nullptr;
    DkimVerifier *m_dkimVerifier = nullptr;
    quint64 m_dkimNextRequest = 0;

    /// Fills in Message-IDs for rows cached before the column existed. Small
    /// chunks on a slow timer: the work is one-time and must never be felt.
    QTimer m_msgidBackfillTimer;

    /// True while presenting a message rebuilt from the cache rather than one
    /// straight off the wire. Purely diagnostic: the cached form is a stub with
    /// attachment payloads stripped and re-inserted on read, so it cannot be
    /// assumed byte-identical to what arrived. Logged next to the DKIM verdict
    /// so the two paths can be told apart — see doc/roadmap.md.
    bool m_presentingFromCache = false;
    /// In-flight verifications by request id. QPointer because a detached
    /// window may close while its message is still being checked.
    QHash<quint64, QPointer<MessageContext>> m_dkimPending;
    bool m_detachPending = false; ///< a double-click is waiting for its fetch
    qint64 m_detachUid = -1;      ///< the message that double-click asked for
    QString m_textPreview;
    QList<QPair<QString, qint64>> m_prefetchQueue; ///< (folder, uid) waiting for a background body fetch
    bool m_prefetching = false;
    QList<std::shared_ptr<BodyConn>> m_bodyPool; ///< parallel body-fetch connections
    bool m_bodyPoolBroken = false; ///< server refused extra connections — stop trying
    bool m_connected = false;
    bool m_busy = false;
    QString m_statusText;        ///< breadcrumb shown in the UI (newest first)
    QStringList m_statusTrail;   ///< recent raw messages, newest first (max 3)

    FolderModel m_folderModel;
    MessageListModel m_messageModel;
    MailStore m_store;
};
