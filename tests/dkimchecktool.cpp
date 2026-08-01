// Runs the real DkimVerifier over .eml files exported from the mailo cache,
// reproducing exactly what MailClient::submitDkimVerification() does.
#include "../src/dkimverifier.h"

#include <KMime/Message>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cstdio>

static const char *statusName(DkimResult::Status s)
{
    switch (s) {
    case DkimResult::None: return "NONE";
    case DkimResult::Pass: return "PASS";
    case DkimResult::Fail: return "FAIL";
    case DkimResult::TempError: return "TEMPERROR";
    case DkimResult::PermError: return "PERMERROR";
    case DkimResult::BodyMismatch: return "UNVERIFIED";
    }
    return "?";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    DkimVerifier verifier;

    for (int i = 1; i < argc; ++i) {
        QFile f(QString::fromLocal8Bit(argv[i]));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray stored = f.readAll(); // exactly what bodies.raw holds
        const QByteArray wire = KMime::LFtoCRLF(stored);

        // From: domain, same as MailClient does.
        QString fromDomain;
        static const QRegularExpression fromRe(
            QStringLiteral("^From:.*?([A-Za-z0-9._%+-]+)@([A-Za-z0-9.-]+)"),
            QRegularExpression::MultilineOption);
        const auto m = fromRe.match(QString::fromLatin1(wire.left(20000)));
        if (m.hasMatch())
            fromDomain = m.captured(2).toLower();

        DkimResult result;
        QObject::connect(&verifier, &DkimVerifier::finished, &app,
                         [&result](quint64, const DkimResult &r) { result = r; });
        verifier.verify(1, wire, fromDomain);

        printf("%-16s from=%-22s d=%-20s %-10s %s\n",
               qPrintable(QFileInfo(f).fileName()), qPrintable(fromDomain),
               qPrintable(result.domain), statusName(result.status),
               qPrintable(result.detail));
    }
    return 0;
}
