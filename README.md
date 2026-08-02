# Mailo

The fast KDE-only email client.

(c) 2026 Daniel Duris, dusoft@staznosti.sk

## What it does

Security-minded KDE-only IMAP mail client, blazing fast.

- **IMAP mail reading** — any server (SSL/TLS, STARTTLS, plain), password or OAuth 2 for Gmail and Microsoft 365. Several accounts at once, reorderable by dragging.
- **Tabs** — composer, Settings and opened messages are tabs, not scattered windows. Ctrl+W closes; the composer can be its own window if preferred.
- **Local cache with full-text search** — headers, read bodies and folders in SQLite: folders open instantly, offline included. IMAP SEARCH → FTS5 (accent-folding) → regex; `/pattern/` is a case-insensitive regex.
- **Imported mail** — a Thunderbird directory imports as an offline account. Add servers later to promote it to a live one, archive intact.
- **Sandboxed message viewing** — HTML renders with JavaScript, plugins and local-file access off, off-the-record, and every remote request blocked until the per-message opt-in. Links open in the system browser.
- **Sender authentication verdicts** — DKIM verified in-process, ARC chains validated, server SPF/DKIM/DMARC alongside. Suspicious mail gets a red **!**.
- **Compose & send** — SMTP with rich text, attachments, signature and resumable drafts. The sending address, name and organization are separate from the login.
- **Attachments** — click to open, right-click to save. Stored zstd-compressed and deduplicated outside the database.
- **Keyboard-first** — arrows, Page Up/Down, Home/End, Enter to open, Ctrl+W to close a tab, and the keyboard follows the folder you open.
- **UX to taste** — Look and feel sets row density, background colour and whether the composer is a tab or a window; Shortcuts rebinds every action, including the five colour labels; General sets the date format, refresh interval and cache limits.
- **Fast by design** — a 20 ms limit on the GUI thread, one frame: anything slower runs on a worker. Pages in from the cache as you scroll, prefetches bodies, sorts in memory.
- **Secure credential storage** — passwords and OAuth tokens in KWallet via Qt6Keychain, never a config file.

## Screenshots
<img width="1920" height="1024" alt="0" src="https://github.com/user-attachments/assets/638d2514-dd1b-41fd-901a-850f130fe2cc" />
<img width="997" height="681" alt="compose" src="https://github.com/user-attachments/assets/4c14c43f-62d2-4bb0-a039-cc86447e5294" />
<img width="899" height="575" alt="2" src="https://github.com/user-attachments/assets/bd762b0c-f9e0-43be-8bb8-c3a9b6d04f08" />
<img width="1198" height="748" alt="1" src="https://github.com/user-attachments/assets/e4fef71e-93d8-4291-b69d-ee0845f24e5b" />
<img width="900" height="573" alt="3" src="https://github.com/user-attachments/assets/386c741a-f78e-420e-9820-8cb2d27acaa7" />
<img width="901" height="576" alt="4" src="https://github.com/user-attachments/assets/980494f3-a786-4da6-aba1-0aadd4540cb5" />

## Installation

Packaged as DEB package and AppImage. Go to https://github.com/nekromoff/mailo/releases (open assets) to download.

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
| Attachment store | content-addressed files, zstd-compressed and deduplicated |
| DKIM / ARC | verified in-process against OpenSSL (libcrypto), worker thread |
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

`build/viewertest` is a headless end-to-end test of the sandboxed viewer pipeline (scheme registration, handler, render).

## Data locations

- Message cache: `~/.local/share/mailo/mailo.db`
- Settings: `~/.config/mailo/mailo.conf` (no secrets)
- Passwords and OAuth refresh tokens: KWallet, service `mailo`

## Status

Working and in daily use. Multiple accounts, OAuth for Gmail and Microsoft 365, imported offline archives, and caches into the tens of gigabytes.
