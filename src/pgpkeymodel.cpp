// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "pgpkeymodel.h"

#include <QLocale>

#include <algorithm>

PgpKeyModel::PgpKeyModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (PgpEngine *engine = PgpEngine::instance()) {
        connect(engine, &PgpEngine::keysChanged, this, &PgpKeyModel::reload);
        reload();
    }
}

int PgpKeyModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> PgpKeyModel::roleNames() const
{
    return {
        {FingerprintRole, "fingerprint"}, {KeyIdRole, "keyId"},
        {NameRole, "name"},               {EmailRole, "email"},
        {UidRole, "uid"},                 {CreatedRole, "created"},
        {ExpiresRole, "expires"},         {ExpiryTextRole, "expiryText"},
        {AlgorithmRole, "algorithm"},     {SecretRole, "secret"},
        {BadRole, "bad"},                 {StatusTextRole, "statusText"},
        {TrustRole, "trust"},
        {OwnerTrustRole, "ownerTrust"},             {CanEncryptRole, "canEncrypt"},
        {CanSignRole, "canSign"},
    };
}

namespace
{
/// What the key manager's second line says about a key. The bad states come
/// first and are stated plainly: a revoked key that merely looked "untrusted"
/// would read as a key one could still choose to use.
QString statusTextFor(const PgpKey &k)
{
    if (k.revoked)
        return PgpKeyModel::tr("Revoked");
    // Not expiry: that is what expiryText says, and saying it here too gave
    // rows reading "Expired · Expired". An expired key still has a trust
    // level, and it is shown the same way as any other key's.
    if (k.disabled)
        return PgpKeyModel::tr("Disabled");
    if (k.invalid)
        return PgpKeyModel::tr("Invalid");
    // A trust level the user assigned wins over the validity gpg computed,
    // because it is the more specific fact and the one they can act on. It
    // also has to be shown *somewhere*: assigning Full to a key with no
    // certification path leaves the computed validity at unknown, so without
    // this the row would still read "Unverified" and the setting would look
    // like it had done nothing.
    switch (k.ownerTrust) {
    case 5:
        return PgpKeyModel::tr("Ultimate trust");
    case 4:
        return PgpKeyModel::tr("Fully trusted");
    case 3:
        return PgpKeyModel::tr("Marginally trusted");
    case 2:
        return PgpKeyModel::tr("Not trusted");
    default:
        break; // nothing assigned — fall through to what gpg worked out
    }
    // GpgME's validity scale, in the words gpg itself uses for it.
    switch (k.validity) {
    case 5:
        return PgpKeyModel::tr("Ultimate trust");
    case 4:
        return PgpKeyModel::tr("Fully trusted");
    case 3:
        return PgpKeyModel::tr("Marginally trusted");
    case 2:
        return PgpKeyModel::tr("Not trusted");
    default:
        // Unknown and undefined both mean the same thing to a reader: nothing
        // has been said about whether this key belongs to whom it claims.
        return PgpKeyModel::tr("Unverified");
    }
}

QString expiryTextFor(const PgpKey &k)
{
    if (k.expired)
        return PgpKeyModel::tr("Expired");
    if (!k.expires.isValid())
        return PgpKeyModel::tr("Never expires");
    return PgpKeyModel::tr("Expires %1")
        .arg(QLocale().toString(k.expires.date(), QLocale::ShortFormat));
}
}

QVariant PgpKeyModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const PgpKey &k = m_rows.at(index.row());
    switch (role) {
    case FingerprintRole:
        return k.fingerprint;
    case KeyIdRole:
        return k.keyId;
    case NameRole:
        return k.name;
    case EmailRole:
        return k.email;
    case UidRole:
        return k.uid;
    case CreatedRole:
        return k.created;
    case ExpiresRole:
        return k.expires;
    case ExpiryTextRole:
        return expiryTextFor(k);
    case AlgorithmRole:
        return k.algorithm;
    case SecretRole:
        return k.secret;
    case BadRole:
        return k.isBad();
    case StatusTextRole:
        return statusTextFor(k);
    case TrustRole:
        return k.validity;
    case OwnerTrustRole:
        return k.ownerTrust;
    case CanEncryptRole:
        return k.canEncrypt;
    case CanSignRole:
        return k.canSign;
    default:
        return {};
    }
}

QVariantMap PgpKeyModel::keyAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    QVariantMap m = m_rows.at(row).toVariantMap();
    m.insert(QStringLiteral("statusText"), statusTextFor(m_rows.at(row)));
    m.insert(QStringLiteral("expiryText"), expiryTextFor(m_rows.at(row)));
    return m;
}

void PgpKeyModel::setSecretOnly(bool on)
{
    if (m_secretOnly == on)
        return;
    m_secretOnly = on;
    reload();
    Q_EMIT filterChanged();
}

void PgpKeyModel::setAddressFilter(const QString &address)
{
    if (m_addressFilter == address)
        return;
    m_addressFilter = address;
    reload();
    Q_EMIT filterChanged();
}

void PgpKeyModel::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    reload();
    Q_EMIT filterChanged();
}

void PgpKeyModel::reload()
{
    PgpEngine *engine = PgpEngine::instance();
    beginResetModel();
    m_rows.clear();
    if (engine) {
        const QString needle = m_searchText.trimmed();
        for (const PgpKey &k : engine->keys()) {
            if (m_secretOnly && !k.secret)
                continue;
            if (!m_addressFilter.isEmpty() && !k.matches(m_addressFilter))
                continue;
            if (!needle.isEmpty()) {
                // Fingerprints are read out in spaced groups but stored solid,
                // so a pasted-in fingerprint has to match either way.
                const QString flat = QString(needle).remove(QLatin1Char(' '));
                const bool hit =
                    k.uid.contains(needle, Qt::CaseInsensitive)
                    || k.fingerprint.contains(flat, Qt::CaseInsensitive)
                    || std::any_of(k.addresses.cbegin(), k.addresses.cend(),
                                   [&needle](const QString &a) {
                                       return a.contains(needle, Qt::CaseInsensitive);
                                   })
                    // Subkey fingerprints and IDs too: they are what gpg
                    // prints in every "encrypted to"/"signed by" line, so
                    // they are what a user pastes into this box.
                    || std::any_of(k.subkeyIds.cbegin(), k.subkeyIds.cend(),
                                   [&flat](const QString &sub) {
                                       return sub.contains(flat, Qt::CaseInsensitive);
                                   });
                if (!hit)
                    continue;
            }
            m_rows.append(k);
        }
    }
    endResetModel();
    Q_EMIT countChanged();
}
