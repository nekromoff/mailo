# Mailo

A fast, security-minded IMAP mail client for the KDE desktop.

(c) 2026 Daniel Duris, dusoft@staznosti.sk

## What it does

- **IMAP mail reading** — connect to any IMAP server (SSL/TLS, STARTTLS, or plain),
  browse the folder tree, read messages. Auth is PLAIN/LOGIN (no OAuth yet, so use
  an app password with providers that require XOAUTH2).
- **Local cache with full-text search** — headers, read message bodies, and the folder
  list are cached in SQLite. Folders open instantly (offline included), previously read
  messages open with zero network, and search falls back through: server-side IMAP
  SEARCH → local FTS5 full-text index → regex over the loaded list. Queries wrapped in
  slashes (`/pattern/`) are treated as a case-insensitive regex.
- **Sandboxed message viewing** — HTML mail renders in QtWebEngine with JavaScript,
  plugins, and local-file access disabled, an off-the-record profile, and a request
  interceptor that blocks all remote content (tracking pixels included) until the
  per-message **Load remote content** opt-in. Inline `cid:` images are served locally.
  Links open in the system browser, never inside the viewer.
- **Sender authentication verdicts** — SPF/DKIM/DMARC results (parsed from the
  receiving server's `Authentication-Results` header) flag suspicious messages with a
  red **!** column in the message list; hover for the raw verdict.
- **Compose & send** — SMTP sending (KSMTP) with a basic rich-text editor: bold,
  italic, font size, ordered/unordered lists, attachments. Messages are sent as
  `multipart/mixed` with a `text/plain` + `text/html` alternative pair.
- **Attachments** — listed in a bottom panel; click to open with the system handler,
  right-click to save into `~/Downloads` (auto-deduplicated filenames).
- **Fast by design** — scroll-based pagination (100 headers at a time), hover and
  read-ahead body prefetching into the cache, keyboard navigation (arrows, Enter,
  pane switching), sortable Subject/From/Date columns.
- **Secure credential storage** — the password lives in KWallet (or any Secret
  Service keyring) via Qt6Keychain, never in a config file.

## Technology

| Layer | Choice |
|---|---|
| Language / toolkit | C++20, Qt 6.11 (QML/Quick) |
| UI framework | KDE Kirigami 6 + Kirigami Addons |
| IMAP | KPim6 KIMAP (async KJobs, no Akonadi) |
| MIME parsing/building | KPim6 KMime |
| SMTP | KPim6 KSMTP |
| HTML viewer | QtWebEngine (Quick), custom `mailo:` URL scheme + request interceptor |
| Storage | SQLite via Qt SQL (WAL), FTS5 for full-text indexing |
| Secrets | Qt6Keychain → KWallet / libsecret |
| Rich-text editing | QTextDocument/QTextCursor exposed to QML (`DocumentHandler`) |
| Build | CMake + Ninja |

## Building

```bash
sudo apt install cmake ninja-build extra-cmake-modules qt6-webengine-dev \
  kf6-kmime-dev kpim6-kimap-dev kpim6-ksmtp-dev qtkeychain-qt6-dev \
  qt6-base-dev qt6-declarative-dev kf6-kirigami-dev

cmake -B build -G Ninja
cmake --build build
./build/mailo
```

`build/viewertest` is a headless end-to-end test of the sandboxed viewer pipeline
(scheme registration, handler, render).

## Data locations

- Message cache: `~/.local/share/mailo/mailo.db`
- Settings: `~/.config/mailo/mailo.conf` (no secrets)
- Password: KWallet, service `mailo`, key `imap-password`

## Status / roadmap

Early but usable single-account client. Not yet implemented: OAuth (Gmail/O365),
IMAP IDLE push, UIDVALIDITY tracking, message flags sync back to server (read state
is local), folder management, threading, GPG/S-MIME.
