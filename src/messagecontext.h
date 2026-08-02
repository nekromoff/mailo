// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace KMime
{
class Content;
class Message;
}
class MailClient;
class ViewerSchemeHandler;

/**
 * One on-screen message: the parsed state behind a MessageViewer.
 *
 * The reading pane owns one long-lived instance (Mail.readingContext);
 * every detached message window gets its own via double-click. Each context
 * keeps the KMime message (and thus the attachment payloads) alive and holds
 * its own slot in the ViewerSchemeHandler, so windows keep rendering — and
 * keep serving inline images, attachments, Reply/Forward — no matter what
 * the main list moves on to.
 *
 * The state is populated by MailClient (friend); the Q_INVOKABLEs delegate
 * back into MailClient, where the composition and attachment logic lives.
 */
class MessageContext : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasMessage READ hasMessage NOTIFY messageChanged)
    /// Identifies the message this context holds — account, folder and uid.
    /// The tab strip uses it to recognise a message it already has open.
    Q_PROPERTY(QString sourceKey READ sourceKey NOTIFY messageChanged)
    Q_PROPERTY(QString subject READ subject NOTIFY messageChanged)
    Q_PROPERTY(QString from READ from NOTIFY messageChanged)
    Q_PROPERTY(QString to READ to NOTIFY messageChanged)
    Q_PROPERTY(QString cc READ cc NOTIFY messageChanged)
    Q_PROPERTY(QString date READ date NOTIFY messageChanged)
    Q_PROPERTY(QString authInfo READ authInfo NOTIFY messageChanged)
    /// The view to load when the message (first) appears — HTML, or plain
    /// text for junk-folder mail.
    Q_PROPERTY(QString bodyUrl READ bodyUrl NOTIFY messageChanged)
    /// [{name, sizeText}, …] of this message's attachments.
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY messageChanged)
    /// True when the message came from a junk/spam folder: plain text by
    /// default, HTML only on explicit request.
    Q_PROPERTY(bool junkTextOnly READ junkTextOnly NOTIFY messageChanged)
    /// Per-message opt-in for remote images/CSS/fonts (persisted per sender).
    Q_PROPERTY(bool remoteContentAllowed READ remoteContentAllowed
                   WRITE setRemoteContentAllowed NOTIFY remoteContentAllowedChanged)
    /// DKIM verification we performed ourselves, as opposed to authInfo, which
    /// is only what the receiving server said. "" while still checking, then
    /// one of none/pass/fail/temperror/permerror.
    Q_PROPERTY(QString dkimStatus READ dkimStatus NOTIFY dkimChanged)
    /// Short human-readable reason, for the tooltip.
    Q_PROPERTY(QString dkimDetail READ dkimDetail NOTIFY dkimChanged)
    /// The only property a "verified" badge may key off: a valid signature
    /// AND a signing domain that matches the sender. A valid-but-unaligned
    /// signature is what a forger's own signature looks like.
    Q_PROPERTY(bool dkimTrusted READ dkimTrusted NOTIFY dkimChanged)
    /// True between opening the message and the verifier answering.
    Q_PROPERTY(bool dkimChecking READ dkimChecking NOTIFY dkimChanged)
    /// ARC chain validation (RFC 8617), checked only when DKIM could not give
    /// the reader an answer to rely on. "" when not checked, otherwise one of
    /// none/pass/sealsonly/fail/error.
    Q_PROPERTY(QString arcStatus READ arcStatus NOTIFY dkimChanged)
    /// Domain of the outermost seal — the party whose word the chain rests on.
    /// A chain says nothing on its own; it says what this domain vouches for.
    Q_PROPERTY(QString arcSealer READ arcSealer NOTIFY dkimChanged)
    /// Short human-readable reason, for the tooltip.
    Q_PROPERTY(QString arcDetail READ arcDetail NOTIFY dkimChanged)

public:
    explicit MessageContext(MailClient *client);
    ~MessageContext() override;

    bool hasMessage() const { return m_hasMessage; }
    QString sourceKey() const { return m_sourceKey; }
    QString subject() const { return m_subject; }
    QString from() const { return m_from; }
    QString to() const { return m_to; }
    QString cc() const { return m_cc; }
    QString date() const { return m_date; }
    QString authInfo() const { return m_authInfo; }
    QString bodyUrl() const { return m_bodyUrl; }
    QVariantList attachments() const { return m_attachments; }
    bool junkTextOnly() const { return m_junk; }
    bool remoteContentAllowed() const { return m_remoteAllowed; }
    void setRemoteContentAllowed(bool allow);
    QString dkimStatus() const { return m_dkimStatus; }
    QString dkimDetail() const { return m_dkimDetail; }
    bool dkimTrusted() const { return m_dkimTrusted; }
    bool dkimChecking() const { return m_dkimChecking; }
    QString arcStatus() const { return m_arcStatus; }
    QString arcSealer() const { return m_arcSealer; }
    QString arcDetail() const { return m_arcDetail; }

    // View URLs for the HTML / Text / Source toggle (this message's, always —
    // independent of what the reading pane shows).
    Q_INVOKABLE QString htmlViewUrl();
    Q_INVOKABLE QString textViewUrl();
    Q_INVOKABLE QString sourceViewUrl();

    /// Compose prefill for replying to this message — see MailClient::replyData.
    Q_INVOKABLE QVariantMap replyData(bool replyAll);
    /// Compose prefill for forwarding this message — see MailClient::forwardData.
    Q_INVOKABLE QVariantMap forwardData();

    Q_INVOKABLE bool attachmentRisky(int index) const;
    Q_INVOKABLE void openAttachment(int index);
    Q_INVOKABLE void saveAttachmentToDownloads(int index);
    Q_INVOKABLE void saveAttachment(int index, const QUrl &fileUrl);

    /// Back to "no message" (reading pane only — windows just close).
    Q_INVOKABLE void clear();
    /// Frees the context when its window closes: drops the scheme-handler
    /// slot (body + inline parts) and deletes this object.
    Q_INVOKABLE void release();

Q_SIGNALS:
    /// A different message (or none) is now behind this context.
    void messageChanged();
    void remoteContentAllowedChanged();
    void dkimChanged();

private:
    friend class MailClient;

    /// The scheme-handler slot, allocated on first use.
    quint64 viewerContext();
    /// Sets the flag without persisting a per-sender preference.
    void applyRemoteAllowed(bool allow);

    MailClient *m_client = nullptr;
    QPointer<ViewerSchemeHandler> m_handler;
    quint64 m_viewerContext = 0; // 0 = not allocated yet

    std::shared_ptr<KMime::Message> m_message; ///< keeps attachment parts alive
    QList<KMime::Content *> m_attachmentParts; ///< owned by m_message
    QVariantList m_attachments;
    QString m_htmlBody;  ///< raw HTML part
    QString m_textBody;  ///< plain-text part
    QByteArray m_raw;    ///< complete RFC-822 source
    qint64 m_uid = -1;
    QString m_sourceKey; ///< account + folder + uid; see sourceKey()
    QString m_senderAddress; ///< addr-spec of the sender (remote-content key)
    bool m_junk = false;
    bool m_remoteAllowed = false;
    bool m_hasMessage = false;

    QString m_subject, m_from, m_to, m_cc, m_date, m_authInfo, m_bodyUrl;

    QString m_dkimStatus;
    QString m_dkimDetail;
    QString m_arcStatus;
    QString m_arcSealer;
    QString m_arcDetail;
    bool m_dkimTrusted = false;
    bool m_dkimChecking = false;
    int m_dkimAttempt = 0; ///< DNS retries used for this message so far
};
