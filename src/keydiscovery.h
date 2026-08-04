// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

/**
 * Finds a public key for an address, without importing anything itself.
 *
 * Two sources, kept apart because their privacy costs are not the same
 * (doc/openpgp.md §7):
 *
 * - **WKD** asks the address's own domain. Mailing someone already tells their
 *   domain that much, so this may run automatically.
 * - **keys.openpgp.org** tells a third party who the user is about to write
 *   to, so it runs on an explicit click only. The old SKS pool is not queried
 *   at all — it serves flooded keys and verifies no addresses.
 *
 * Results come back as raw key blocks for PgpEngine to import; this class
 * never touches the keyring.
 */
class KeyDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit KeyDiscovery(QObject *parent = nullptr);

    /// WKD lookup for \a address. Runs through gpg's own dirmngr, which
    /// implements both the advanced and the direct method, the z-base-32
    /// hashing and the fallbacks — reimplementing that over QNetworkAccessManager
    /// would be a second, worse WKD client to keep correct.
    void lookupWkd(const QString &address);

    /// keys.openpgp.org's VKS lookup, over plain HTTPS to that host and no
    /// other. Not dirmngr's keyserver path: that would query whatever server
    /// the user's gpg.conf names, which for many installations is still a
    /// pool we deliberately do not use.
    void lookupKeyserver(const QString &address);

Q_SIGNALS:
    /// One lookup finished. \a keyData empty with \a error empty means the
    /// address simply has no published key — a normal answer, not a failure.
    /// \a source names where it came from, for the UI to say so.
    void finished(const QString &address, const QByteArray &keyData,
                  const QString &source, const QString &error);

private:
    QNetworkAccessManager *m_net = nullptr;
};
