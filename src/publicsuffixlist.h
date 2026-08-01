// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QString>

class QNetworkAccessManager;

/**
 * The Public Suffix List, used to work out organizational domains.
 *
 * DMARC relaxed alignment compares the *organizational* domain of the signature
 * against that of the From: address, and there is no algorithm for that — only
 * this list. Without it `mail.example.co.uk` and `news.example.co.uk` look
 * unrelated, and `example.co.uk` versus `other.co.uk` look like parent and
 * child. The first mistake rejects legitimate mail; the second would accept a
 * forgery, which is why the fallback used when the list is unavailable errs
 * only in the first direction (see domainsAligned()).
 *
 * Threading: the list is read from the DKIM verifier's thread and refreshed on
 * the GUI thread, so every read goes through a shared lock. Reads are frequent
 * but tiny (a handful of hash lookups per message); refreshes are weekly.
 */
class PublicSuffixList : public QObject
{
    Q_OBJECT

public:
    /// One instance for the process; the rules are several thousand entries and
    /// there is no reason for two copies of them.
    static PublicSuffixList &instance();

    /// Loads the cached copy from disk, then downloads a new one if what we
    /// have is missing or more than a week old, and keeps checking weekly for
    /// as long as the process runs. Call once, from the GUI thread, before
    /// anything else touches the instance — the networking it sets up belongs
    /// to whichever thread gets here first. Returns immediately; the download
    /// swaps the rules in when it lands.
    void start();

    /// The registrable domain of \a domain — one label more than its public
    /// suffix. Returns an empty string when the list is not loaded, or when
    /// \a domain *is* a public suffix and so has no organization behind it.
    /// Safe to call from any thread.
    QString organizationalDomain(const QString &domain) const;

    /// True once a list has been parsed, from disk or from the network.
    bool isLoaded() const;

    /// Parses the list format (rules, !exceptions, *.wildcards, // comments).
    /// Exposed for tests; replaces whatever is loaded.
    void setRulesFromData(const QByteArray &data);

private:
    using QObject::QObject;

    void loadFromDisk();
    void download();
    static QString cachePath();

    mutable QReadWriteLock m_lock;
    /// Suffix rules split by kind. Hashed whole rather than by label count:
    /// lookup walks the candidate suffixes of a name from longest to shortest,
    /// which is at most as many probes as the name has labels.
    QSet<QString> m_rules;      ///< ordinary rules, e.g. "co.uk"
    QSet<QString> m_wildcards;  ///< "*.foo" stored as "foo"
    QSet<QString> m_exceptions; ///< "!bar.foo" stored as "bar.foo"

    QNetworkAccessManager *m_nam = nullptr;
    bool m_downloading = false;
};
