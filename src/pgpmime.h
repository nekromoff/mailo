// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

namespace KMime
{
class Content;
}

/**
 * RFC 3156 structure, and nothing else.
 *
 * Pure functions over a parsed KMime tree: what shape is this message, and
 * which bytes are the ciphertext / the signed octets / the signature. No
 * keyring, no network, no gpg — which is what makes it testable on fixed
 * vectors (tests/pgpmimetest.cpp) rather than against whatever keys the
 * machine happens to hold.
 *
 * The classification is deliberately conservative. Anything that is not
 * unambiguously "this whole message is encrypted" comes back as Partial or
 * None, because the alternative — rendering decrypted and plaintext parts
 * under one badge — is the classic MIME-mixing spoof (doc/openpgp.md §3).
 */
namespace PgpMime
{

enum class Kind {
    None,             ///< no OpenPGP structure
    Encrypted,        ///< RFC 3156 multipart/encrypted at the top level
    Signed,           ///< RFC 3156 multipart/signed at the top level
    InlineEncrypted,  ///< legacy armored PGP MESSAGE that *is* the whole body
    /// OpenPGP somewhere inside a larger message: an encrypted sub-part of a
    /// multipart/mixed, or armor with plaintext around it. Never decrypted for
    /// display — see the note above.
    Partial,
};

struct Structure {
    Kind kind = Kind::None;
    /// The node that was classified — kept so the signed part's octets can be
    /// found again in the raw message, which needs the multipart boundary.
    KMime::Content *root = nullptr;
    /// multipart/encrypted part 2 — application/octet-stream, the ciphertext.
    KMime::Content *cipherPart = nullptr;
    /// multipart/signed part 1 — the content the signature covers.
    KMime::Content *signedPart = nullptr;
    /// multipart/signed part 2 — application/pgp-signature.
    KMime::Content *signaturePart = nullptr;
    /// The armored block, for InlineEncrypted (which has no part of its own).
    QByteArray inlineBlock;

    bool isEncrypted() const
    {
        return kind == Kind::Encrypted || kind == Kind::InlineEncrypted;
    }
};

/// Examines \a root — already parsed — and reports its OpenPGP shape.
/// \a root is not modified.
Structure classify(KMime::Content *root);

/// The bytes to hand to the decrypter for \a s, or empty when there are none.
QByteArray ciphertext(const Structure &s);

/// The octets the signature covers, re-serialised from the parsed tree.
///
/// Only a fallback: re-serialising can refold or reorder headers, and a
/// signature checked against those bytes proves nothing when it fails. Prefer
/// the overload below wherever the message's own bytes are to hand.
QByteArray signedOctets(const Structure &s);

/// The exact octets the signature covers, sliced out of \a raw — the message
/// as it arrived — rather than rebuilt from the parsed tree.
///
/// RFC 3156 signs the first part of the multipart/signed byte for byte, CRLF
/// intact, so this walks \a raw to the MIME boundary and returns what lies
/// between the first two delimiters, untouched. That is the difference between
/// a mismatch that means "this message was altered" and one that means "we
/// could not reproduce what was signed" (doc/openpgp.md §3, doc/roadmap.md).
///
/// Falls back to the re-serialised form when the boundary cannot be found in
/// \a raw — which is what happens for a cached body that was reassembled from
/// externalised attachments. \a exact, when given, says which of the two it
/// returned, so the caller knows whether a mismatch is worth reporting.
QByteArray signedOctets(const QByteArray &raw, const Structure &s, bool *exact = nullptr);

/// The armored (or binary) signature of \a s.
QByteArray signature(const Structure &s);

/// What \a head — raw message headers, before any parse — declares: Encrypted,
/// Signed, or None. Cheap enough for the fetch path, where the body may not
/// have arrived yet; the same trick MailStore::headIndicatesAttachment uses.
/// Never reports inline PGP, which is only visible in the body, and never
/// Partial, which needs the structure.
Kind kindFromHead(const QByteArray &head);

/// True when \a data is a MIME entity — headers, a blank line, then a body —
/// rather than message text on its own.
///
/// Decrypting PGP/MIME yields an entity. Decrypting *inline* PGP yields the
/// text the sender wrote, because that is all inline PGP ever encrypted. The
/// two have to be told apart before parsing: hand a bare paragraph to KMime
/// and it reads the first line as a header and leaves the body empty, which is
/// how a perfectly good decryption ends up displaying nothing at all.
bool looksLikeMimeEntity(const QByteArray &data);

/// An assembled message split into the headers that identify it and the MIME
/// content that carries it. RFC 3156 wraps only the second half: From, To,
/// Subject and Date stay on the outside, in the clear, whatever is done to the
/// body.
struct OutgoingParts {
    QByteArray identityHeaders; ///< From/To/Subject/… , one per line, CRLF
    QByteArray contentPart;     ///< Content-* headers, blank line, body
    bool valid = false;
};

/// Splits \a assembled — a complete message in wire form — for wrapping.
OutgoingParts splitForCrypto(const QByteArray &assembled);

/// Builds an RFC 3156 multipart/signed from \a parts and the detached
/// \a signature over parts.contentPart, naming \a micalg ("pgp-sha256").
///
/// The content part is copied in **verbatim**. Nothing re-assembles it, because
/// re-assembly is exactly what refolds headers and breaks the signature that
/// was just made over these bytes — the sending-side face of the octet problem
/// in doc/roadmap.md.
QByteArray buildSigned(const OutgoingParts &parts, const QByteArray &signature,
                       const QString &micalg);

/// Builds an RFC 3156 multipart/encrypted around \a armoredCipher.
QByteArray buildEncrypted(const OutgoingParts &parts, const QByteArray &armoredCipher);

/// The `messages.crypto` column values (doc/openpgp.md §8). Deliberately a
/// plain int in the database: it is read by the message list, which must not
/// depend on anything OpenPGP.
enum StoredKind { StoredNone = 0, StoredEncrypted = 1, StoredSigned = 2, StoredBoth = 3 };

/// \a kind as the `crypto` column stores it.
int storedKind(Kind kind);

}
