// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "spamheuristics.h"

#include "publicsuffixlist.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace SpamHeuristics
{

namespace
{

/// One header field, unfolded. Kept as a list rather than a map because order
/// matters for Received: the topmost one is the last hop, and that is the only
/// one in the chain a sender cannot have written.
struct Field {
    QString name; ///< lowercased
    QString value;
};

/// Splits a raw header block into unfolded fields. Tolerant on purpose: spam is
/// frequently malformed, and refusing to parse it would be a way of not
/// noticing it.
QList<Field> parseHead(const QByteArray &head)
{
    QList<Field> out;
    const QString text = QString::fromUtf8(head);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;
        // Continuation of the previous field.
        if ((line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t')))
            && !out.isEmpty()) {
            out.last().value += QLatin1Char(' ') + line.trimmed();
            continue;
        }
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        out.append({line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed()});
    }
    return out;
}

QString firstValue(const QList<Field> &fields, QLatin1String name)
{
    for (const Field &f : fields) {
        if (f.name == name)
            return f.value;
    }
    return {};
}

bool hasField(const QList<Field> &fields, QLatin1String name)
{
    for (const Field &f : fields) {
        if (f.name == name)
            return true;
    }
    return false;
}

int countFields(const QList<Field> &fields, QLatin1String name)
{
    int n = 0;
    for (const Field &f : fields) {
        if (f.name == name)
            ++n;
    }
    return n;
}

const QRegularExpression &addressRe()
{
    static const QRegularExpression re(
        QStringLiteral("[A-Za-z0-9._%+'-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"));
    return re;
}

/// The script of a letter, reduced to the three alphabets that matter for
/// homograph attacks. Everything else — CJK, Arabic, Hebrew — is left alone:
/// those are not confusable with Latin, and treating a Japanese subject line as
/// evidence of fraud would be both wrong and offensive.
enum class Script { Other, Latin, Cyrillic, Greek };

Script scriptOf(QChar c)
{
    if (!c.isLetter())
        return Script::Other;
    switch (c.script()) {
    case QChar::Script_Latin:
        return Script::Latin;
    case QChar::Script_Cyrillic:
        return Script::Cyrillic;
    case QChar::Script_Greek:
        return Script::Greek;
    default:
        return Script::Other;
    }
}

/// True when a single word mixes Latin with Cyrillic or Greek letters. Mixing
/// across a whole subject is ordinary in multilingual mail; mixing *inside one
/// word* is how "PayPaI" and "аpple.com" are built, and essentially never
/// happens by accident.
bool hasConfusableWord(const QString &s)
{
    const QStringList words = s.split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    for (const QString &word : words) {
        QSet<int> scripts;
        for (const QChar c : word) {
            const Script sc = scriptOf(c);
            if (sc != Script::Other)
                scripts.insert(static_cast<int>(sc));
        }
        if (scripts.size() > 1)
            return true;
    }
    return false;
}

/// Strips tags and collapses whitespace, to estimate how much text a reader
/// actually sees in an HTML part.
QString visibleText(const QString &html)
{
    QString s = html;
    s.remove(QRegularExpression(QStringLiteral("<(script|style)[^>]*>.*?</\\1>"),
                                QRegularExpression::CaseInsensitiveOption
                                    | QRegularExpression::DotMatchesEverythingOption));
    s.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    s.replace(QRegularExpression(QStringLiteral("&nbsp;?"), QRegularExpression::CaseInsensitiveOption),
              QStringLiteral(" "));
    return s.simplified();
}

QString hostOfUrl(const QString &url)
{
    QString s = url;
    const int scheme = s.indexOf(QStringLiteral("://"));
    if (scheme >= 0)
        s = s.mid(scheme + 3);
    s = s.section(QLatin1Char('/'), 0, 0).section(QLatin1Char('?'), 0, 0);
    s = s.section(QLatin1Char('@'), -1);          // strip userinfo
    s = s.section(QLatin1Char(':'), 0, 0);        // strip port
    return s.toLower();
}

QString orgOfDomain(const QString &domain)
{
    if (domain.isEmpty())
        return {};
    const QString org = PublicSuffixList::instance().organizationalDomain(domain);
    // Falling back to the full domain can only make two names look less
    // related than they are. That direction produces a missed rule, never a
    // false accusation, which is the trade we want when the list is absent.
    return org.isEmpty() ? domain : org;
}

} // namespace

QString normalizeAddress(const QString &address)
{
    QString a = address.trimmed().toLower();
    const int at = a.lastIndexOf(QLatin1Char('@'));
    if (at <= 0)
        return a;
    QString local = a.left(at);
    const int plus = local.indexOf(QLatin1Char('+'));
    if (plus > 0)
        local = local.left(plus);
    return local + a.mid(at);
}

namespace
{

/// Splits a mailbox header value into display name and addr-spec, honouring
/// quoting and taking the *last* angle-addr.
///
/// Getting this wrong is a security bug rather than a cosmetic one. Given
///
///     From: "Alice <alice@known.example>" <thief@evil.test>
///
/// the naive answer — first address anywhere in the value — is
/// alice@known.example, which is both the wrong sender and, far worse, an
/// allowlist lookup the sender got to choose the result of. Rule 0 would then
/// exempt any message whose display name names someone the user has written
/// to, which is a string anybody can type.
void splitMailbox(const QString &value, QString *name, QString *addr)
{
    bool inQuotes = false;
    int open = -1;
    int close = -1;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('\\')) {
            ++i; // escaped character, whatever it is
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (inQuotes)
            continue;
        if (c == QLatin1Char('<')) {
            open = i;
            close = -1;
        } else if (c == QLatin1Char('>') && open >= 0 && close < 0) {
            close = i;
        }
    }

    if (open < 0) {
        // Bare addr-spec, no angle brackets and so no display name.
        *name = QString();
        const auto m = addressRe().match(value);
        *addr = m.hasMatch() ? m.captured(0) : QString();
        return;
    }

    const QString inner = close > open ? value.mid(open + 1, close - open - 1)
                                       : value.mid(open + 1);
    const auto m = addressRe().match(inner);
    *addr = m.hasMatch() ? m.captured(0) : QString();

    QString shown = value.left(open).trimmed();
    if (shown.size() >= 2 && shown.startsWith(QLatin1Char('"'))
        && shown.endsWith(QLatin1Char('"'))) {
        shown = shown.mid(1, shown.size() - 2);
    }
    *name = shown.trimmed();
}

} // namespace

QString addressOf(const QString &headerValue)
{
    QString name;
    QString addr;
    splitMailbox(headerValue, &name, &addr);
    return addr.isEmpty() ? QString() : normalizeAddress(addr);
}

QString displayNameOf(const QString &headerValue)
{
    QString name;
    QString addr;
    splitMailbox(headerValue, &name, &addr);
    return name;
}

QString organizationalDomainOf(const QString &address)
{
    const int at = address.lastIndexOf(QLatin1Char('@'));
    if (at < 0)
        return {};
    return orgOfDomain(address.mid(at + 1).toLower());
}

QString Score::explanation() const
{
    QStringList lines;
    for (const Hit &h : hits)
        lines.append(QStringLiteral("%1 (%2)").arg(h.detail).arg(h.weight, 0, 10));
    return lines.join(QLatin1Char('\n'));
}

Score score(const Message &msg, const Context &ctx)
{
    Score out;
    const QList<Field> fields = parseHead(msg.head);

    const QString fromValue = firstValue(fields, QLatin1String("from"));
    const QString fromAddr = addressOf(fromValue);
    const QString fromOrg = organizationalDomainOf(fromAddr);

    // ------------------------------------------------------------------
    // Rule 0: someone the user has written to is not a spammer.
    //
    // The one thing that can revoke it is the message failing authentication
    // at our own receiving server. That is not a weakening of the rule — a
    // message claiming to be from a known contact while failing SPF/DKIM/DMARC
    // is precisely *not* from that contact, and an unconditional allowlist
    // would hand a free pass to anyone who guesses an address the user has
    // mailed. Absent auth data (no trusted Authentication-Results at all) the
    // exemption stands: no evidence is not evidence of forgery.
    // ------------------------------------------------------------------
    if (ctx.knownCorrespondent && !ctx.authFailed && !ctx.alwaysScore) {
        out.exempt = true;
        out.verdict = Verdict::Ham;
        out.exemptReason = QStringLiteral("You have sent mail to %1.").arg(fromAddr);
        return out;
    }

    auto hit = [&out](const char *id, int weight, const QString &detail) {
        out.hits.append({QString::fromLatin1(id), weight, detail});
        out.total += weight;
    };

    // --- Authentication ------------------------------------------------
    if (ctx.knownCorrespondent && ctx.authFailed) {
        // Decisive on its own: forging an address the user actually corresponds
        // with is targeted, not incidental.
        hit("known-contact-spoofed", 60,
            QStringLiteral("Claims to be %1, whom you have written to, but sender "
                           "authentication failed — the address is probably forged")
                .arg(fromAddr));
    } else if (ctx.authFailed) {
        hit("auth-fail", 35,
            QStringLiteral("Receiving server reported an SPF/DKIM/DMARC failure"));
    }
    if (ctx.authPassed && !ctx.authFailed)
        hit("auth-pass", -20, QStringLiteral("Sender authentication passed"));

    // --- OpenPGP -------------------------------------------------------
    // No key is checked here; the MIME shape alone is enough. Spam does not
    // arrive signed or encrypted, so this is a reliable ham signal even before
    // any signature has been verified.
    if (ctx.crypto == 2 || ctx.crypto == 3)
        hit("pgp-signed", -50, QStringLiteral("Message is OpenPGP signed"));
    else if (ctx.crypto == 1)
        hit("pgp-encrypted", -40, QStringLiteral("Message is OpenPGP encrypted"));

    // --- From / display name -------------------------------------------
    const QString displayName = displayNameOf(fromValue);
    if (!displayName.isEmpty() && !fromOrg.isEmpty()) {
        const auto m = addressRe().match(displayName);
        if (m.hasMatch()) {
            const QString shownOrg = organizationalDomainOf(normalizeAddress(m.captured(0)));
            if (!shownOrg.isEmpty() && shownOrg != fromOrg) {
                hit("display-name-address", 30,
                    QStringLiteral("Sender name shows %1 but the message is from %2")
                        .arg(m.captured(0), fromAddr));
            }
        }
        if (hasConfusableWord(displayName)) {
            hit("display-name-confusable", 30,
                QStringLiteral("Sender name mixes alphabets within a word: \"%1\"")
                    .arg(displayName));
        }
    }

    // --- Reply-To ------------------------------------------------------
    // Only meaningful when authentication did not pass: plenty of legitimate
    // senders route replies elsewhere (support desks, mailing lists), so on its
    // own this is weak and must never be able to mark anything.
    const QString replyTo = firstValue(fields, QLatin1String("reply-to"));
    if (!replyTo.isEmpty() && !ctx.authPassed) {
        const QString replyOrg = organizationalDomainOf(addressOf(replyTo));
        if (!replyOrg.isEmpty() && !fromOrg.isEmpty() && replyOrg != fromOrg) {
            hit("reply-to-mismatch", 12,
                QStringLiteral("Replies would go to %1, not %2").arg(replyOrg, fromOrg));
        }
    }

    // --- Message-ID ----------------------------------------------------
    const QString msgid = firstValue(fields, QLatin1String("message-id"));
    if (msgid.isEmpty()) {
        hit("no-message-id", 18,
            QStringLiteral("No Message-ID — normal mail software always writes one"));
    } else {
        const int at = msgid.lastIndexOf(QLatin1Char('@'));
        if (at > 0) {
            QString midHost = msgid.mid(at + 1);
            midHost.remove(QLatin1Char('>')).remove(QLatin1Char('"'));
            const QString midOrg = orgOfDomain(midHost.trimmed().toLower());
            if (!midOrg.isEmpty() && !fromOrg.isEmpty() && midOrg != fromOrg && !ctx.authPassed) {
                hit("msgid-domain-mismatch", 8,
                    QStringLiteral("Message-ID was issued by %1, not %2").arg(midOrg, fromOrg));
            }
        }
    }

    // --- Relay chain ---------------------------------------------------
    const int received = countFields(fields, QLatin1String("received"));
    if (received == 0) {
        hit("no-received", 20,
            QStringLiteral("No Received headers — the message did not travel through "
                           "any mail server we can see"));
    }

    // --- Date skew -----------------------------------------------------
    // Compared against the topmost Received, which our own server wrote: a
    // sender can lie about Date but not about when we took delivery.
    const QString dateValue = firstValue(fields, QLatin1String("date"));
    const QString topReceived = firstValue(fields, QLatin1String("received"));
    if (!dateValue.isEmpty() && !topReceived.isEmpty()) {
        const QDateTime claimed = QDateTime::fromString(dateValue, Qt::RFC2822Date);
        const QString stamp = topReceived.section(QLatin1Char(';'), -1).trimmed();
        const QDateTime actual = QDateTime::fromString(stamp, Qt::RFC2822Date);
        if (claimed.isValid() && actual.isValid()) {
            const qint64 skewHours = qAbs(claimed.secsTo(actual)) / 3600;
            // Two days of slack: clock drift, batch senders and timezone-naive
            // software all produce small skews on perfectly good mail.
            if (skewHours > 48) {
                hit("date-skew", 15,
                    QStringLiteral("Date header is %1 hours away from when the message "
                                   "actually arrived").arg(skewHours));
            }
        }
    }

    // --- Subject -------------------------------------------------------
    const QString subject = firstValue(fields, QLatin1String("subject"));
    if (!subject.isEmpty()) {
        // Bidi overrides let a subject render as something other than what it
        // says. There is no legitimate use of these in a subject line.
        static const QString bidiControls = QStringLiteral("\u202A\u202B\u202D\u202E\u2066\u2067\u2068\u202C\u2069");
        for (const QChar c : subject) {
            if (bidiControls.contains(c)) {
                hit("subject-bidi-override", 30,
                    QStringLiteral("Subject contains a text-direction override, which "
                                   "makes it display differently from what it says"));
                break;
            }
        }
        if (hasConfusableWord(subject)) {
            hit("subject-confusable", 25,
                QStringLiteral("Subject mixes alphabets within a word — a common way to "
                               "imitate a brand name"));
        }
        // Shouting alone is tacky, not criminal; weighted so it can only ever
        // push something already suspicious over the line.
        int letters = 0;
        int upper = 0;
        for (const QChar c : subject) {
            if (!c.isLetter())
                continue;
            ++letters;
            if (c.isUpper())
                ++upper;
        }
        if (letters >= 12 && upper * 10 >= letters * 8)
            hit("subject-shouting", 6, QStringLiteral("Subject is almost entirely capitals"));
    }

    // --- Bulk mail shape -----------------------------------------------
    const bool listId = hasField(fields, QLatin1String("list-id"));
    const bool unsub = hasField(fields, QLatin1String("list-unsubscribe"));
    const QString precedence = firstValue(fields, QLatin1String("precedence")).toLower();
    if ((listId || precedence == QLatin1String("bulk")) && !unsub) {
        hit("bulk-no-unsubscribe", 10,
            QStringLiteral("Sent as bulk mail but offers no List-Unsubscribe header"));
    }

    // --- Body ----------------------------------------------------------
    // Every rule below is skipped when no body is available, which is the
    // normal case while the message list is being built. Nothing here is
    // load-bearing on its own.
    //
    // Deliberately absent: hidden text (font-size:0, display:none). It is the
    // textbook spam signal and also exactly how every marketing platform on
    // earth hides an inbox preheader, so it fires on a large fraction of
    // perfectly wanted mail. There is no weight at which it is worth having.
    if (!msg.html.isEmpty()) {
        const QString seen = visibleText(msg.html);
        static const QRegularExpression imgRe(QStringLiteral("<img\\b"),
                                              QRegularExpression::CaseInsensitiveOption);
        if (seen.size() < 80 && msg.html.contains(imgRe)) {
            hit("image-only", 12,
                QStringLiteral("Almost all of the message is one image, with no text a "
                               "filter could read"));
        }

        // An anchor whose *text* names a domain different from where it goes.
        // Only counted when the text is domain-shaped: ordinary link text
        // ("click here", a product name) says nothing either way.
        static const QRegularExpression anchorRe(
            QStringLiteral("<a\\b[^>]*href\\s*=\\s*[\"']?(https?://[^\"'\\s>]+)[\"']?[^>]*>(.*?)</a>"),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression domainTextRe(
            QStringLiteral("^(?:https?://)?([A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+)/?$"));
        auto it = anchorRe.globalMatch(msg.html);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString shown = visibleText(m.captured(2));
            const auto dm = domainTextRe.match(shown);
            if (!dm.hasMatch())
                continue;
            const QString shownOrg = orgOfDomain(dm.captured(1).toLower());
            const QString realOrg = orgOfDomain(hostOfUrl(m.captured(1)));
            if (!shownOrg.isEmpty() && !realOrg.isEmpty() && shownOrg != realOrg) {
                hit("link-text-mismatch", 25,
                    QStringLiteral("A link reading \"%1\" actually goes to %2")
                        .arg(shown, realOrg));
                break; // one is enough; ten of them is not ten times the evidence
            }
        }
    }

    out.verdict = out.total >= SpamThreshold  ? Verdict::Spam
        : out.total >= UnsureThreshold        ? Verdict::Unsure
                                              : Verdict::Ham;
    return out;
}

} // namespace SpamHeuristics
