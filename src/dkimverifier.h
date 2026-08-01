// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

/**
 * Outcome of verifying one message's DKIM signatures.
 *
 * \a aligned is not decoration. A valid signature only proves that whoever
 * controls \a domain signed the message — an attacker can sign their own mail
 * with their own domain and get Pass. The result is only meaningful to a user
 * once \a domain is checked against the From: header domain, so any UI that
 * says "verified" must require aligned == true.
 */
struct DkimResult {
    enum Status {
        None,      ///< no DKIM-Signature header at all
        Pass,      ///< at least one signature verified
        Fail,      ///< the signature did not match the published key
        TempError, ///< DNS lookup failed; retrying later may succeed
        PermError, ///< malformed signature, unusable key, unsupported algorithm
        /// The body hash did not match. Deliberately NOT Fail: this happens
        /// whenever our own copy of the message is not byte-identical to what
        /// arrived, and measured against real cached mail that is currently
        /// the common case — for most messages our body hash matches neither
        /// the sender's bh= nor the independent one Gmail recorded in
        /// ARC-Message-Signature. Until the fetch path preserves the original
        /// octets (see doc/roadmap.md) a mismatch cannot be distinguished from
        /// tampering, and announcing "signature invalid" on good mail is the
        /// same class of error as trusting a forged Authentication-Results.
        BodyMismatch,
    };

    Status status = None;
    QString domain;   ///< d= of the signature this result describes
    QString selector; ///< s= of that signature
    bool aligned = false;
    QString detail; ///< short human-readable reason, for the tooltip

    bool trustworthy() const { return status == Pass && aligned; }
};

/**
 * Canonicalization primitives (RFC 6376 §3.4).
 *
 * Exposed only so tests can check them against the RFC's own vectors in
 * §3.4.5. This is where DKIM verifiers usually go wrong: a canonicalization
 * that is subtly off rejects legitimate mail rather than failing loudly, so it
 * needs external ground truth rather than a round-trip against our own signer.
 */
namespace DkimCanon
{
QByteArray headerRelaxed(const QByteArray &name, const QByteArray &value);
/// Takes the field's original bytes: "simple" must not normalize anything.
QByteArray headerSimple(const QByteArray &rawField);
QByteArray bodyRelaxed(const QByteArray &body);
QByteArray bodySimple(const QByteArray &body);
}

/**
 * Verifies DKIM signatures (RFC 6376) against the DNS-published public key.
 *
 * Lives on its own thread and must only be driven through queued calls to
 * verify(): the work is a DNS round trip plus SHA-256 over the whole message
 * and a public-key operation, none of which belongs on the GUI thread.
 * Verification is deliberately not run for every header in the message list —
 * only for a message the user actually opens. That keeps the cost off the sync
 * path and avoids emitting a DNS query for every sender who has ever mailed
 * you, which would leak the shape of the mailbox to the resolver.
 *
 * The caller passes the message as it appeared on the wire (CRLF line endings);
 * see MailClient::rawMessageForDkim().
 */
class DkimVerifier : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

public Q_SLOTS:
    /// \a rawMessageCrlf must be the original octets, CRLF-terminated.
    /// \a fromDomain is the domain of the From: header, for the alignment test.
    /// Answers with finished(\a requestId, …) exactly once.
    void verify(quint64 requestId, const QByteArray &rawMessageCrlf, const QString &fromDomain);

public:
    /// Test seam: supplies the DKIM key record that would otherwise come from
    /// DNS, so verification can be exercised without a resolver. Never set in
    /// the application.
    void setKeyRecordForTest(const QByteArray &record) { m_testKeyRecord = record; }

Q_SIGNALS:
    void finished(quint64 requestId, const DkimResult &result);

private:
    struct Signature; // one parsed DKIM-Signature header

    /// Everything except the DNS step; returns false with \a out filled in when
    /// the signature is unusable before we ever need the key.
    bool prepare(const QByteArray &head, const QByteArray &body, const Signature &sig,
                 QByteArray *signedData, DkimResult *out) const;

    QByteArray m_testKeyRecord;
};

Q_DECLARE_METATYPE(DkimResult)
