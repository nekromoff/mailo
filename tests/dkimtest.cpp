// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

// Canonicalization is where DKIM verifiers go wrong, and they go wrong quietly:
// a subtly incorrect canonical form rejects legitimate mail instead of failing
// loudly. These vectors come from RFC 6376 §3.4.5, so they are external ground
// truth rather than a round-trip against our own code.
//
// Exit 0 = all vectors match.

#include "../src/dkimverifier.h"
#include "../src/publicsuffixlist.h"

#include <QByteArray>
#include <cstdio>

namespace
{
int failures = 0;

QByteArray visible(QByteArray in)
{
    in.replace("\r\n", "<CRLF>").replace('\t', QByteArrayLiteral("<HTAB>"));
    in.replace(' ', QByteArrayLiteral("<SP>"));
    return in;
}

void check(const char *what, const QByteArray &got, const QByteArray &want)
{
    if (got == want) {
        printf("  ok   %s\n", what);
        return;
    }
    ++failures;
    printf("  FAIL %s\n", what);
    printf("        got  %s\n", visible(got).constData());
    printf("        want %s\n", visible(want).constData());
}
} // namespace

int main()
{
    // The RFC's example message:
    //     A: <SP> X <CRLF>
    //     B <SP> : <SP> Y <HTAB><CRLF>
    //     <HTAB> Z <SP><SP><CRLF>
    //     <CRLF>
    //     <SP> C <SP><CRLF>
    //     D <SP><HTAB><SP> E <CRLF>
    //     <CRLF>
    //     <CRLF>
    const QByteArray body = " C \r\nD \t E\r\n\r\n\r\n";

    printf("relaxed header canonicalization (RFC 6376 3.4.5)\n");
    check("A: X", DkimCanon::headerRelaxed("A", " X"), "a:X\r\n");
    check("folded B", DkimCanon::headerRelaxed("B", " Y\t\r\n\tZ  "), "b:Y Z\r\n");

    printf("simple header canonicalization\n");
    // "simple" changes nothing at all — not the case, not the odd space before
    // the colon, not the trailing whitespace.
    check("A: X unchanged", DkimCanon::headerSimple("A: X"), "A: X\r\n");
    check("folded B unchanged", DkimCanon::headerSimple("B : Y\t\r\n\tZ  "),
          "B : Y\t\r\n\tZ  \r\n");

    printf("relaxed body canonicalization\n");
    // Leading WSP on a line is reduced to one space, NOT deleted.
    check("example body", DkimCanon::bodyRelaxed(body), " C\r\nD E\r\n");
    check("empty body", DkimCanon::bodyRelaxed(""), "");
    check("only blank lines", DkimCanon::bodyRelaxed("\r\n\r\n"), "");

    printf("simple body canonicalization\n");
    check("example body", DkimCanon::bodySimple(body), " C \r\nD \t E\r\n");
    check("empty body", DkimCanon::bodySimple(""), "\r\n");
    check("no trailing CRLF gains one", DkimCanon::bodySimple("x"), "x\r\n");

    // The other half of "quietly wrong": alignment. An organizational domain
    // computed one label off either rejects legitimate mail or accepts a
    // forgery, and the rules that make it non-obvious are exactly the ones
    // below. Vectors come from the algorithm's own test list at
    // publicsuffix.org/list/, not from our parser.
    printf("public suffix lookups\n");
    PublicSuffixList::instance().setRulesFromData(
        "// a cut-down list with one of each rule shape\n"
        "com\n"
        "uk\n"
        "co.uk\n"
        "jp\n"
        "kobe.jp\n"
        "*.kobe.jp\n"
        "!city.kobe.jp\n"
        "*.ck\n"
        "!www.ck\n");
    auto org = [](const char *name) {
        return PublicSuffixList::instance().organizationalDomain(QString::fromLatin1(name));
    };
    check("plain TLD", org("example.com").toLatin1(), "example.com");
    check("subdomain", org("mail.example.com").toLatin1(), "example.com");
    // The case the old parent/child heuristic got wrong in the safe direction.
    check("siblings share an org domain", org("a.example.co.uk").toLatin1(), "example.co.uk");
    check("a public suffix has no owner", org("co.uk").toLatin1(), "");
    check("unlisted TLD falls back to '*'", org("example.invalidtld").toLatin1(),
          "example.invalidtld");
    check("wildcard rule", org("www.city.kobe.jp").toLatin1(), "city.kobe.jp");
    check("exception overrules its wildcard", org("www.ck").toLatin1(), "www.ck");
    // A wildcard makes the label under it part of the suffix, so the owned name
    // starts one label further left than it looks.
    check("wildcard swallows a label", org("bar.ck").toLatin1(), "");
    check("owned name under a wildcard", org("foo.bar.ck").toLatin1(), "foo.bar.ck");
    check("deeper name under a wildcard", org("a.foo.bar.ck").toLatin1(), "foo.bar.ck");

    if (failures == 0) {
        printf("PASS: all canonicalization vectors match\n");
        return 0;
    }
    printf("FAIL: %d vector(s) wrong\n", failures);
    return 1;
}
