# JMAP Roadmap

Plan for adding JMAP as a second protocol backend alongside IMAP (drafted 2026-07-19).

**Starting point:** `src/mailclient.cpp` is a single QML-facing controller where UI
logic and KIMAP protocol code are interleaved, and `src/mailstore.cpp` assumes
IMAP's numeric UID/UIDVALIDITY model. JMAP is a different world — JSON over
HTTPS, string ids, state-string delta sync, push via Server-Sent Events, and
sending built into the protocol (no SMTP). So: refactor first, then an
incremental JMAP backend.

## Phase 0 — Extract a backend interface (no behavior change)

Split `MailClient` into:

- **`MailClient`** stays the QML "Mail" singleton: account registry, models,
  viewer/attachment handling, compose prefill, settings — everything that isn't
  protocol traffic.
- **`MailBackend`** (abstract QObject): `connectAccount()`, `listFolders()`,
  `openFolder()`, `fetchHeaders(window)`, `fetchBody(id)`, `setFlags()`,
  `moveToTrash()/expunge()`, `search()`, `appendSent()/send()`,
  `startPush()/stopPush()`, plus signals mirroring today's flow
  (`foldersListed`, `headersFetched`, `bodyFetched`, `pushEvent`, …).
- **`ImapBackend`**: the existing KIMAP code (three-session setup, backfill
  cursor, IDLE) moved verbatim. The `withSyncSession`/backfill machinery is an
  IMAP implementation detail and stays inside it.

Riskiest phase only in the "large mechanical refactor" sense; must end with
zero user-visible change, verified against the real account.

Message identity becomes backend-neutral here: the store's `qint64 uid` gets a
companion `TEXT remote_id` (IMAP writes the uid as text, JMAP writes its Email
id), and per-folder sync metadata becomes an opaque blob (`uidvalidity` for
IMAP, JMAP `state` strings). Small `MailStore` migration under the existing
`meta_flags` pattern.

## Phase 1 — JMAP core + read-only backend

No KDE framework exists for JMAP; hand-rolled on `QNetworkAccessManager`
(async, fits the existing no-threads event-loop model and the
UI-responsiveness rule).

- **`JmapSession`**: fetch the session object from
  `https://host/.well-known/jmap` (auto-discovery — the account sheet needs
  only address + credentials), hold `apiUrl`/`downloadUrl`/`uploadUrl`/
  `eventSourceUrl` and the mail account id. Auth: Basic and Bearer (existing
  `oauthhelper.cpp` tokens work as-is for Fastmail-style servers).
- **`JmapRequest`**: one method-call batch = one POST; thin wrapper handling
  back-references, error mapping, and retry-on-401.
- **`JmapBackend`** read path:
  - `Mailbox/get` → folder tree (mailbox *roles* replace the by-name
    trash/sent detection in `trashFolderName()`).
  - `Email/query` (sorted by `receivedAt`, windowed by `position`) +
    `Email/get` for header pages — replaces the entire IMAP backfill-cursor
    dance; one connection suffices since HTTP isn't serialized like a KIMAP
    session.
  - Blob download for full bodies, parsed by the existing KMime path so the
    viewer/attachment code is untouched (JMAP serves the original RFC-822
    blob).
- Account model: add `protocol` to per-account settings and a protocol picker
  in `AccountSheet.qml` that hides the SMTP/port/security rows for JMAP.

**Exit criteria:** add a JMAP account, browse folders, read mail, all cached in
MailStore/FTS5 like IMAP mail.

## Phase 2 — Write operations + sending

- Flags/read state and delete-to-trash via `Email/set` (`mailboxIds` patch);
  expunge via `Email/set destroy`.
- Sending via `Identity/get` + blob upload + `EmailSubmission/set` — no SMTP
  involved; `onSuccessUpdateEmail` files the sent copy server-side, replacing
  `appendToSentFolder()`.
- Compose UI unchanged; `sendMail()` routes to the active backend.

## Phase 3 — Delta sync, push, search

- `Email/changes` / `Mailbox/changes` with stored state strings — cheap
  incremental refresh replacing the "fetch newer than cache" logic.
- Push via the session's EventSource URL (SSE over a streaming
  `QNetworkReply`, with reconnect/backoff) — JMAP's answer to IDLE; covers all
  folders at once, not just the open one.
- Server search via `Email/query` with a `filter` (text/from/subject/body
  match the existing search-field enum); the local `/regex/` filter path is
  backend-neutral already.

## Testing & risks

- Test server: **Stalwart** (single binary, first-class JMAP) locally;
  Fastmail as the real-world target. Nothing gets installed without sign-off —
  a Stalwart binary or container is the one prerequisite.
- Main risks: the Phase 0 refactor touches ~2,600 lines of working sync code
  (mitigate: pure code movement, verify against the live account before any
  JMAP work), and MailStore id-model assumptions may leak beyond the schema
  (the `\x1f`-keyed row scheme itself is fine).
- Rough shape: Phases 0–1 are the bulk of the work; 2–3 are comparatively
  small since JMAP makes write/sync/push simpler than IMAP.
