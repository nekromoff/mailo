// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include "pgpengine.h"

/// The keyring as a list, for the key manager and the account key picker.
///
/// A view of PgpEngine's snapshot, not a second copy of the keyring: the
/// engine is the only thing that talks to gpg, and every instance of this
/// model reloads from the same snapshot when it changes. Instances are created
/// in QML, each with its own filter.
class PgpKeyModel : public QAbstractListModel
{
    Q_OBJECT
    /// Only keys whose private half is present — the user's own identities.
    Q_PROPERTY(bool secretOnly READ secretOnly WRITE setSecretOnly NOTIFY filterChanged)
    /// Only keys carrying this address in a user ID. Empty = no address filter.
    Q_PROPERTY(QString addressFilter READ addressFilter WRITE setAddressFilter
                   NOTIFY filterChanged)
    /// Free-text filter over name, address and fingerprint (the search field in
    /// the key manager). Empty = everything.
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY filterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        FingerprintRole = Qt::UserRole + 1,
        KeyIdRole,
        NameRole,
        EmailRole,
        UidRole,
        CreatedRole,
        ExpiresRole,
        ExpiryTextRole,   ///< "never", "12 Mar 2027", or "expired"
        AlgorithmRole,
        SecretRole,
        BadRole,          ///< expired, revoked, disabled or invalid
        StatusTextRole,   ///< why it is bad, or the trust level, in one word
        TrustRole,        ///< 0-5, GpgME's computed validity scale
        OwnerTrustRole,   ///< 0-5, what the user assigned (settable)
        CanEncryptRole,
        CanSignRole,
    };
    Q_ENUM(Roles)

    explicit PgpKeyModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool secretOnly() const { return m_secretOnly; }
    void setSecretOnly(bool on);
    QString addressFilter() const { return m_addressFilter; }
    void setAddressFilter(const QString &address);
    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);

    /// The row's key as a map, for callers that want several fields at once.
    Q_INVOKABLE QVariantMap keyAt(int row) const;

Q_SIGNALS:
    void filterChanged();
    void countChanged();

private:
    void reload();

    QList<PgpKey> m_rows;
    QString m_addressFilter;
    QString m_searchText;
    bool m_secretOnly = false;
};
