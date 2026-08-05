#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
# SPDX-License-Identifier: LGPL-3.0-or-later
"""
The smallest JMAP server that can prove push works: a session object and an
EventSource stream, and nothing else.

It exists because the cyrus-jmap-tester container answers 204 to
/jmap/eventsource — its httpd was built without push — so the one thing the
live tests cannot reach is the whole of `JmapBackend`'s push path. The pure
parts (framing, field parsing, StateChange matching) are covered offline in
jmapbackendtest; this covers the plumbing between them: the request headers,
the streaming reply, and the buffer that spans reads.

It deliberately sends the events awkwardly — split mid-line, across a CRLF, with
comments and a ping in between — because that is what a real network does and
what the framing code is written to survive.

    python3 tests/data/jmap-push-stub.py 18081 &
    ./build/tests/jmapbackendtest --live-push http://127.0.0.1:18081
"""

import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18081

SESSION = """{
  "username": "stub@example.invalid",
  "apiUrl": "/jmap/api/",
  "downloadUrl": "/jmap/download/{accountId}/{blobId}/{name}?accept={type}",
  "uploadUrl": "/jmap/upload/{accountId}/",
  "eventSourceUrl": "/jmap/eventsource/?types={types}&closeafter={closeafter}&ping={ping}",
  "state": "0",
  "capabilities": {
    "urn:ietf:params:jmap:core": {"maxSizeUpload": 50000000, "maxCallsInRequest": 16},
    "urn:ietf:params:jmap:mail": {}
  },
  "accounts": {
    "acc1": {"name": "stub", "isPersonal": true, "isReadOnly": false,
             "accountCapabilities": {"urn:ietf:params:jmap:mail": {}}}
  },
  "primaryAccounts": {"urn:ietf:params:jmap:mail": "acc1"}
}"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("stub: " + (fmt % args) + "\n")

    def do_GET(self):
        if self.path.startswith("/.well-known/jmap") or self.path.startswith("/jmap/session"):
            body = SESSION.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path.startswith("/jmap/eventsource/"):
            self.stream_events()
            return

        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def stream_events(self):
        # The client must ask for a stream and authenticate like any other
        # request; a stub that ignored both would pass a client that forgot.
        if "text/event-stream" not in self.headers.get("Accept", ""):
            self.log_message("no event-stream Accept header")
            self.send_response(400)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if not self.headers.get("Authorization"):
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        def write(chunk):
            self.wfile.write(chunk.encode() if isinstance(chunk, str) else chunk)
            self.wfile.flush()

        try:
            # A comment first: servers send these to hold the connection open,
            # and a client that treats one as an event would fire early.
            write(": welcome\n\n")
            time.sleep(0.2)
            # An explicit ping event, which must also not count as news.
            write("event: ping\ndata: {\"interval\":300}\n\n")
            time.sleep(0.2)
            # Another account's change — not ours, must be ignored.
            write('event: state\ndata: {"@type":"StateChange",'
                  '"changed":{"other":{"Email":"s9"}}}\n\n')
            time.sleep(0.2)
            # The real one, delivered in three pieces with the second break
            # falling *between* a CR and its LF.
            write('event: state\ndata: {"@type":"StateChange",')
            time.sleep(0.2)
            write('"changed":{"acc1":{"Email":"s1"}}}\r')
            time.sleep(0.2)
            write("\nid: evt-1\r\n\r\n")
            # Stay open so the client sees a live stream rather than a close.
            while True:
                time.sleep(1)
                write(": ping\n\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write("stub: listening on 127.0.0.1:%d\n" % PORT)
    server.serve_forever()
