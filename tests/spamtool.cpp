// Diagnostic, not a pass/fail test: runs the real spam heuristics over .eml
// files and prints the score with every rule that fired. This is how the
// weights in spamheuristics.cpp get tuned — without a corpus to measure
// against, a spam filter's false-positive rate is a guess.
//
//   ./spamtool msg.eml ...                  score each message
//   ./spamtool --dir mail/                  score every .eml under a directory
//   ./spamtool --ham ham/ --spam spam/      confusion matrix over two corpora
//   ./spamtool --known alice@example.com    treat these senders as allowlisted
//   ./spamtool --auth-fail msg.eml          score as if SPF/DKIM/DMARC failed
//   ./spamtool --auth-pass msg.eml          ...or passed
//   ./spamtool --crypto 2 msg.eml           score as OpenPGP signed (1 enc, 2 sig, 3 both)
//   ./spamtool --quiet ...                  totals only, no per-message lines
//
// Bodies are used when the file has them, so the same message can score
// differently here and in the message list, which only ever sees headers.
#include "../src/publicsuffixlist.h"
#include "../src/spamheuristics.h"

#include <KMime/Message>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

namespace
{

const char *verdictName(SpamHeuristics::Verdict v)
{
    switch (v) {
    case SpamHeuristics::Verdict::Ham: return "HAM";
    case SpamHeuristics::Verdict::Unsure: return "UNSURE";
    case SpamHeuristics::Verdict::Spam: return "SPAM";
    }
    return "?";
}

/// Pulls the text/plain and text/html bodies out of a parsed message. Uses
/// KMime rather than a hand-rolled split so the tool sees what the client
/// would see once body scoring is wired into the viewer.
void collectBodies(KMime::Content *node, QString *text, QString *html)
{
    if (!node)
        return;
    const auto children = node->contents();
    if (!children.isEmpty()) {
        for (KMime::Content *child : children)
            collectBodies(child, text, html);
        return;
    }
    const QByteArray mime =
        node->contentType() ? node->contentType()->mimeType().toLower() : QByteArray();
    if (mime == "text/html" && html->isEmpty())
        *html = node->decodedText();
    else if (mime == "text/plain" && text->isEmpty())
        *text = node->decodedText();
}

struct Totals {
    int ham = 0;
    int unsure = 0;
    int spam = 0;
    int exempt = 0;
    void count(const SpamHeuristics::Score &s)
    {
        if (s.exempt)
            ++exempt;
        switch (s.verdict) {
        case SpamHeuristics::Verdict::Ham: ++ham; break;
        case SpamHeuristics::Verdict::Unsure: ++unsure; break;
        case SpamHeuristics::Verdict::Spam: ++spam; break;
        }
    }
    int total() const { return ham + unsure + spam; }
};

/// Scores one file. Returns false when it could not be read.
bool scoreFile(const QString &path, const QSet<QString> &known, bool alwaysScore,
               bool authFailed, bool authPassed, int crypto, bool quiet, Totals *totals)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", qPrintable(path));
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    KMime::Message msg;
    msg.setContent(KMime::CRLFtoLF(raw));
    msg.parse();

    SpamHeuristics::Message m;
    m.head = msg.head();
    collectBodies(&msg, &m.text, &m.html);

    SpamHeuristics::Context ctx;
    ctx.alwaysScore = alwaysScore;
    ctx.crypto = crypto;
    const QString from = msg.from() ? msg.from()->asUnicodeString() : QString();
    const QString addr = SpamHeuristics::addressOf(from);
    ctx.knownCorrespondent = known.contains(addr);

    // The message's own Authentication-Results is deliberately NOT read. In the
    // client only a header stamped by our own receiving server counts, and here
    // there is no "our server" to compare an authserv-id against — trusting the
    // file's own header would measure the filter against a value the sender
    // controls. --auth-fail / --auth-pass simulate the verdict instead, which
    // is the only way to exercise the known-contact-spoofed rule offline.
    ctx.authFailed = authFailed;
    ctx.authPassed = authPassed;

    const SpamHeuristics::Score s = SpamHeuristics::score(m, ctx);
    totals->count(s);

    if (quiet)
        return true;

    std::printf("%-40s %-7s %4d  %s\n", qPrintable(QFileInfo(path).fileName()),
                verdictName(s.verdict), s.total, qPrintable(addr));
    if (s.exempt) {
        std::printf("      exempt: %s\n", qPrintable(s.exemptReason));
        return true;
    }
    for (const SpamHeuristics::Hit &h : s.hits) {
        std::printf("      %+4d %-26s %s\n", h.weight, qPrintable(h.id),
                    qPrintable(h.detail));
    }
    return true;
}

QStringList emlsUnder(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, {QStringLiteral("*.eml")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out.append(it.next());
    out.sort();
    return out;
}

void printTotals(const char *label, const Totals &t)
{
    if (t.total() == 0)
        return;
    std::printf("%s: %d messages — ham %d, unsure %d, spam %d (%d exempt under Rule 0)\n",
                label, t.total(), t.ham, t.unsure, t.spam, t.exempt);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Loads the cached Public Suffix List and refreshes it if stale. Without it
    // organizationalDomainOf() falls back to the full domain, which only makes
    // the alignment rules fire less often — the tool still runs, it just
    // under-reports, so say so rather than silently producing softer numbers.
    PublicSuffixList::instance().start();

    QStringList files;
    QStringList hamDirs;
    QStringList spamDirs;
    QSet<QString> known;
    bool alwaysScore = false;
    bool authFailed = false;
    bool authPassed = false;
    int crypto = 0;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const auto next = [&]() -> QString {
            return (i + 1 < argc) ? QString::fromLocal8Bit(argv[++i]) : QString();
        };
        if (arg == QLatin1String("--dir"))
            files += emlsUnder(next());
        else if (arg == QLatin1String("--ham"))
            hamDirs.append(next());
        else if (arg == QLatin1String("--spam"))
            spamDirs.append(next());
        else if (arg == QLatin1String("--known"))
            known.insert(SpamHeuristics::normalizeAddress(next()));
        else if (arg == QLatin1String("--always-score"))
            alwaysScore = true;
        else if (arg == QLatin1String("--auth-fail"))
            authFailed = true;
        else if (arg == QLatin1String("--auth-pass"))
            authPassed = true;
        else if (arg == QLatin1String("--crypto"))
            crypto = next().toInt();
        else if (arg == QLatin1String("--quiet"))
            quiet = true;
        else if (arg.startsWith(QLatin1String("--"))) {
            std::fprintf(stderr, "unknown option %s\n", qPrintable(arg));
            return 2;
        } else {
            files.append(arg);
        }
    }

    if (files.isEmpty() && hamDirs.isEmpty() && spamDirs.isEmpty()) {
        std::fprintf(stderr,
                     "usage: spamtool [--quiet] [--always-score] [--known ADDR]...\n"
                     "                [--auth-fail|--auth-pass] [--crypto 0|1|2|3]\n"
                     "                [--dir DIR] [--ham DIR] [--spam DIR] [FILE...]\n");
        return 2;
    }
    if (!PublicSuffixList::instance().isLoaded()) {
        std::fprintf(stderr, "warning: no Public Suffix List — domain-alignment rules "
                             "will under-report\n");
    }

    Totals plain;
    for (const QString &f : std::as_const(files))
        scoreFile(f, known, alwaysScore, authFailed, authPassed, crypto, quiet, &plain);

    Totals hamTotals;
    for (const QString &d : std::as_const(hamDirs)) {
        for (const QString &f : emlsUnder(d))
            scoreFile(f, known, alwaysScore, authFailed, authPassed, crypto, quiet, &hamTotals);
    }
    Totals spamTotals;
    for (const QString &d : std::as_const(spamDirs)) {
        for (const QString &f : emlsUnder(d))
            scoreFile(f, known, alwaysScore, authFailed, authPassed, crypto, quiet, &spamTotals);
    }

    std::printf("\n");
    printTotals("files", plain);
    printTotals("ham corpus", hamTotals);
    printTotals("spam corpus", spamTotals);

    if (hamTotals.total() > 0 && spamTotals.total() > 0) {
        // False positives are reported first and on their own line because they
        // are the only number that matters much: a missed spam costs a delete,
        // a marked good message costs trust in the whole feature.
        const double fp = 100.0 * hamTotals.spam / hamTotals.total();
        const double fn = 100.0 * spamTotals.ham / spamTotals.total();
        std::printf("\nfalse positives: %d/%d (%.2f%% of good mail marked)\n",
                    hamTotals.spam, hamTotals.total(), fp);
        std::printf("false negatives: %d/%d (%.2f%% of spam unmarked)\n",
                    spamTotals.ham, spamTotals.total(), fn);
        std::printf("caught: %d/%d (%.2f%%)\n", spamTotals.spam, spamTotals.total(),
                    100.0 * spamTotals.spam / spamTotals.total());
    }
    return 0;
}
