// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

// Canonicalization is where DKIM verifiers go wrong, and they go wrong quietly:
// a subtly incorrect canonical form rejects legitimate mail instead of failing
// loudly. These vectors come from RFC 6376 §3.4.5, so they are external ground
// truth rather than a round-trip against our own code.
//
// Exit 0 = all vectors match.

#include "../src/dkimverifier.h"

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

    if (failures == 0) {
        printf("PASS: all canonicalization vectors match\n");
        return 0;
    }
    printf("FAIL: %d vector(s) wrong\n", failures);
    return 1;
}
