// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

/// Checks the guarantees SecureWipe actually makes — and, just as importantly,
/// the one it deliberately does not. Self-contained: no keyring, no network.
///
/// Exit 0 = all checks passed.

#include "securewipe.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QTextStream>

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

namespace
{
QTextStream out(stdout);
int failures = 0;

void check(bool ok, const char *what)
{
    out << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok)
        ++failures;
    out.flush();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // A buffer nobody else holds is emptied.
    {
        QByteArray secret = QByteArrayLiteral("decrypted message body");
        SecureWipe::wipe(secret);
        check(secret.isEmpty(), "wiping a uniquely-owned buffer empties it");
    }
    {
        QString secret = QStringLiteral("decrypted message body");
        SecureWipe::wipe(secret);
        check(secret.isEmpty(), "wiping a uniquely-owned string empties it");
    }

    // The case worth pinning: a shared buffer is NOT overwritten. Qt would
    // detach on the first write, so the wipe would land on a private copy made
    // for the occasion and leave the shared bytes intact — protection that
    // isn't. wipe() drops its reference instead, and this test exists so that
    // behaviour cannot be "fixed" into the silent-failure version by someone
    // who assumes fill() is enough.
    {
        QByteArray secret = QByteArrayLiteral("shared decrypted body");
        QByteArray sharer = secret; // same buffer, refcount 2
        SecureWipe::wipe(secret);
        check(secret.isEmpty(), "wiping a shared buffer still releases our reference");
        check(sharer == QByteArrayLiteral("shared decrypted body"),
              "wiping a shared buffer does not corrupt the other holder");
    }

#ifdef Q_OS_LINUX
    // Core dumps off while plaintext is held, and — the half that is easy to
    // get wrong — back on afterwards, so ordinary crashes stay debuggable.
    {
        const int before = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        SecureWipe::holdPlaintext();
        const int during = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        SecureWipe::releasePlaintext();
        const int after = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        check(during == 0, "core dumps are suppressed while plaintext is held");
        check(after == before, "core dumps are restored once it is released");
    }

    // Nested holds must not restore early: two messages open, one closed, the
    // other still has plaintext in memory.
    {
        SecureWipe::holdPlaintext();
        SecureWipe::holdPlaintext();
        SecureWipe::releasePlaintext();
        const int stillHeld = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        SecureWipe::releasePlaintext();
        const int released = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        check(stillHeld == 0, "a second message keeps dumps suppressed");
        check(released == 1, "the last release restores them");
    }

    // An unbalanced release must not drive the count negative and leave dumps
    // suppressed for the rest of the session.
    {
        SecureWipe::releasePlaintext();
        SecureWipe::holdPlaintext();
        const int during = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        SecureWipe::releasePlaintext();
        check(during == 0, "an unmatched release does not unbalance the count");
    }
#endif

    out << (failures == 0 ? "\nall checks passed\n"
                          : QStringLiteral("\n%1 failure(s)\n").arg(failures));
    out.flush();
    return failures == 0 ? 0 : 1;
}
