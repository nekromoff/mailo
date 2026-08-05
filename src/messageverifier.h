// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

#include "dkimverifier.h"
#include "pgpengine.h"

class MailStore;
class MessageContext;
class MessagePresenter;
class QThread;

namespace KMime
{
class Content;
class Message;
}

/**
 * Decides what a message's signatures are worth: DKIM/ARC on one side,
 * OpenPGP on the other, and the cache-healing rule they share.
 *
 * Both checks answer the same question about the same bytes, and both hit the
 * same trap: a body read back from the offline cache is not always the octets
 * that arrived, so a hash mismatch there is far more likely to be our copy
 * than the sender's. Refetching once and re-checking against the server's
 * copy is the rule that makes either verdict trustworthy, which is why the
 * two live together rather than beside their respective engines.
 *
 * The refetch itself belongs to whoever owns the connection: this class asks
 * for the server's copy through refetchBody() and knows nothing about how it
 * arrives.
 */
class MessageVerifier : public QObject
{
    Q_OBJECT
public:
    /// Which check found a body mismatch — it decides what runs again
    /// afterwards, and which badge shows "checking" meanwhile.
    enum class HealReason { DkimBodyHash, OpenPgpSignature };

    /// Called with the server's copy of the message, or a null pointer when
    /// the refetch ultimately failed.
    using BodyReady = std::function<void(const std::shared_ptr<KMime::Message> &)>;
    /// Asks the connection for \a folder/\a uid's body. Returns false when
    /// there is nothing to ask (offline, or the message is not cached), in
    /// which case \a done is never called. Retrying a backend that declined
    /// the request is the implementation's business, not this class's.
    using RefetchBody = std::function<bool(const QString &folder, qint64 uid,
                                           BodyReady done)>;

    /// \a presenter re-renders a message once it has been decrypted.
    /// \a indexText extracts searchable text for a healed body written back
    /// to the cache.
    MessageVerifier(MailStore &store, MessagePresenter *presenter,
                    std::function<QString(KMime::Message *)> indexText,
                    QObject *parent = nullptr);
    /// Stops and joins the DKIM worker thread.
    ~MessageVerifier() override;

    /// How to get the server's copy — set once, by whoever owns the backend.
    void setRefetchBody(RefetchBody refetch) { m_refetch = std::move(refetch); }
    /// Hands the verifier the OpenPGP backend (main.cpp owns it). Without it —
    /// or with one that reports unavailable — encrypted mail is shown as
    /// encrypted and undecryptable, and nothing else changes.
    void setPgpEngine(PgpEngine *engine);
    /// Master switch for sender authentication. When off nothing is checked
    /// and nothing is shown — no DNS query is emitted at all.
    void setAuthVerification(bool on) { m_authVerification = on; }
    /// A local archive never verifies: the DNS keys its signatures were made
    /// against are long gone, so every check would report a failure that says
    /// nothing about the message.
    void setArchiveAccount(bool local) { m_local = local; }
    /// Whether the message about to be submitted was rebuilt from the cache
    /// rather than taken off the wire. Recorded per submission, because how a
    /// mismatch must be reported depends on where *those* bytes came from.
    void setPresentingFromCache(bool fromCache) { m_presentingFromCache = fromCache; }

    /// Hands \a ctx's message to the DKIM worker thread. Called only when a
    /// message is actually opened — never during sync or prefetch, so the
    /// mailbox does not turn into a stream of DNS queries to the resolver.
    void startDkimVerification(MessageContext *ctx);
    /// Starts detached verification of \a root's RFC 3156 signature, if it has
    /// one, and records the job against \a ctx. Returns true if a job started.
    /// \a bytesFromCache is whether the octets \a root was parsed from came
    /// out of the offline cache — the caller's fact to state, because only the
    /// caller knows where its bytes came from. Ignored for a decrypted tree,
    /// whose plaintext always came out of gpg just now, in this session.
    bool startPgpVerification(MessageContext *ctx, KMime::Message *root,
                              bool bytesFromCache);
    /// Starts decryption of \a ctx's message, recording the job against it.
    void trackDecryptJob(quint64 jobId, MessageContext *ctx);
    /// A detached window joins the job its source context is waiting on,
    /// rather than starting a second decryption (and a second passphrase
    /// prompt) for the same message.
    void adoptJobs(MessageContext *ctx, quint64 decryptJob, quint64 verifyJob);

Q_SIGNALS:
    /// The list's OpenPGP mark can be refined: the head only shows the outer
    /// type, so an encrypted message that turns out to be signed as well is
    /// only known to be both once it has been opened. The model only, never
    /// the store — see doc/openpgp.md §4.
    void cryptoMarkRefined(const QString &folder, qint64 uid, int storedKind);
    /// The decrypted tree turned out to carry attachments, which the head
    /// could not say. Same rule: on screen for this session, never cached.
    void attachmentsDiscovered(const QString &folder, qint64 uid);

private:
    void submitDkimVerification(MessageContext *ctx);
    /// DNS was unreachable rather than authoritative, so the key may well
    /// exist. Backs off and tries again a few times before giving up.
    /// Returns false when the attempts are exhausted.
    bool scheduleDkimRetry(MessageContext *ctx);
    void applyDkimResult(quint64 requestId, const DkimResult &result);
    /// A body hash that fails against a *cached* copy usually says our copy is
    /// stale rather than that the message was altered: bodies written by older
    /// builds are not the octets that arrived (see doc/roadmap.md). Drops the
    /// cached body, refetches it once, and re-verifies against what the server
    /// actually holds. Returns false when healing does not apply, in which
    /// case the mismatch stands as "not verified".
    bool healCachedBody(MessageContext *ctx, HealReason reason);
    void applyVerification(quint64 jobId, const PgpSignatureInfo &signature);
    void applyDecryption(quint64 jobId, const QByteArray &plainText,
                         const QString &error, bool noSecretKey);

    MailStore &m_store;
    MessagePresenter *m_presenter = nullptr;
    std::function<QString(KMime::Message *)> m_indexText;
    RefetchBody m_refetch;

    bool m_authVerification = true;
    bool m_local = false;
    bool m_presentingFromCache = false;

    /// DKIM verification runs off the GUI thread: it is a DNS round trip plus
    /// SHA-256 over the whole message and a public-key operation.
    QThread *m_dkimThread = nullptr;
    DkimVerifier *m_dkimVerifier = nullptr;
    quint64 m_dkimNextRequest = 0;
    /// In-flight verifications by request id. QPointer because a detached
    /// window may close while its message is still being checked.
    QHash<quint64, QPointer<MessageContext>> m_dkimPending;
    /// "folder\nuid" of messages whose cached body has already been refetched
    /// once because its body hash failed. Session-scoped on purpose: a healed
    /// body is byte-exact afterwards, so the only messages that could come
    /// back here are ones that genuinely mismatch, and those must not cost a
    /// fetch on every open.
    QSet<QString> m_dkimHealed;

    /// OpenPGP backend, set by main.cpp. Null, or unavailable, on a machine
    /// without gnupg — every path through here checks before using it.
    QPointer<PgpEngine> m_pgp;
    /// Decrypt jobs in flight, by the token PgpEngine::decrypt() handed out.
    /// A list per job, because detaching a window mid-decryption gives the
    /// same job a second context to answer rather than a second decryption
    /// (and a second passphrase prompt). A reader who moves on before gpg
    /// answers leaves entries nothing wants; applyDecryption() drops those.
    QHash<quint64, QList<QPointer<MessageContext>>> m_pendingDecrypt;
    /// Verify jobs in flight, same scheme as m_pendingDecrypt. A signature
    /// found inside an encrypted message reuses that message's decrypt token,
    /// so an id can appear in both maps.
    QHash<quint64, QList<QPointer<MessageContext>>> m_pendingVerify;
};
