# examples/tlsecho

A small line service over TLS, and a client that will not talk to a server it
can't verify. Together they exercise most of Diamond's TLS surface:
certificate verification, a caller-supplied trust anchor, mutual TLS with
client certificates, ALPN negotiation, session resumption, and binary data
framed behind a length on the same connection as text.

```text
$ diamond tlsecho.di serve 8443 cert.pem key.pem &
listening on tls/8443
$ diamond tlsecho.di client localhost 8443 --ca cert.pem --alpn tlsecho/2,tlsecho/1 \
    'ECHO hello' 'GZIP compress me'
negotiated: tlsecho/2
ECHO hello -> hello
GZIP compress me -> 20 lines of 'compress me', 240 bytes, compressed on the wire: true
```

## Usage

```text
tlsecho.di serve PORT CERT KEY [--client-ca CAFILE]
tlsecho.di client HOST PORT --ca CAFILE [--cert FILE --key FILE] [--alpn LIST]
                 [--resume] COMMAND...
```

Commands: `ECHO text`, `UPPER text`, `PROTO` (the negotiated protocol),
`WHOAMI` (the client certificate's subject the server saw, or `anonymous`),
`GZIP text` (only under `tlsecho/2`; the reply is the text repeated 20 times,
gzip-compressed), `QUIT`. `--alpn` is a comma-separated preference list.
`--resume` reconnects with the first connection's session and prints
`resumed: true` when TLS actually resumed it. `--client-ca` makes the server
demand a client certificate signed by that CA; `--cert`/`--key` are the
client's. The server stops on SIGTERM or
Ctrl+C and reports how many connections it served. Exit status is 0 on
success, 64 for a usage error, and 66 when a connection can't be made
(including a certificate that fails verification).

To try it, make a certificate whose name matches the host you will connect to:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 2 \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1
```

## What it shows

- **Verification you can't switch off.** `TLSSocket.connect` always checks
  the certificate chain and that the certificate is valid for `HOST`; there
  is no `verify: false`. `--ca` chooses *which* trust anchor is consulted
  (the `ca_file` option), nothing more. Connecting with the wrong CA, to a
  name the certificate doesn't cover (`127.0.0.2`), or with no `--ca` against
  a self-signed certificate all raise a rescuable `IOError`, which the client
  turns into exit 66.
- **A server that survives a client's refusal.** When a client rejects the
  certificate it aborts the handshake, so `accept()` raises on the server too.
  The accept loop rescues that, notes `handshake failed`, and carries on.
- **ALPN, decided by the server.** The server offers `tlsecho/2` then
  `tlsecho/1`; whichever the client also offered *first in the server's list*
  wins, regardless of the client's own order. With no ALPN offered,
  `alpn_protocol()` is `nil`, and the service treats that as "no gzip".
- **Text and binary on one connection.** `GZIP` replies with a header line
  `GZ <n>` and then exactly n bytes of gzip data. The client reads the header
  with `gets()` and the body with `read(n)`, looping because a TLS read may
  return fewer bytes than asked for.
- **Session resumption.** `session()` after a first read, `session:` on the
  next `connect`, and `session_reused?()` to confirm it, since a resumption
  that doesn't happen falls back to a full handshake silently.
- **`Signal.trap` while blocked in `accept`.** The handler is a nested `def`
  that reads the live connection counter and calls `exit(0)`.
- **Mutual TLS.** `TLSServer.listen(..., {"client_ca": path})` requests a
  client certificate in every handshake and refuses any connection whose
  certificate doesn't verify against that CA. The refusal raises on the
  server's `accept()` (rescued, so the loop carries on) and reaches the
  client as an `IOError` on its first read or write, since under TLS 1.3 the
  client's own handshake finishes before the server has judged its
  certificate. `WHOAMI` uses `peer_subject()` on the accepted connection to
  say who connected (`O=Acme,CN=alice`, RFC 2253 order); `peer_fingerprint()`
  gives a stable SHA-256 identity to pin. Without `--client-ca` the server
  never asks, `peer_subject()` is `nil`, and `WHOAMI` answers `anonymous`.
- **`gets()` drops the newline**, so every reply here writes its own `"\n"`.

## Test

```sh
bash smoke_test.sh
```

Generates a throwaway certificate with `openssl` (which must be on the PATH),
starts the server as a `diamond build` binary on port 18200 (`TLSECHO_PORT`
overrides it), and checks ALPN preference, the gzip frame, resumption, every
verification refusal, that the server keeps serving afterwards, and that
SIGTERM ends it with exit 0. A second run with `--client-ca` (on the next
port) admits a client certificate from the trusted CA, refuses no certificate
and one from another CA, and checks that a server without `--client-ca`
reports `anonymous`.
