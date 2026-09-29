#!/usr/bin/env bash
# Generates a throwaway certificate with openssl, starts tlsecho as a
# `diamond build` binary, and checks it end to end over real TLS: ALPN
# negotiation (with the server's preference winning), a gzip frame carried
# behind a length on the same connection as text lines, session resumption,
# and that certificate verification cannot be talked out of: an untrusted CA,
# a wrong host name, and the system trust store (which doesn't know this
# certificate) are all refused, and the server carries on. Ends the server
# with SIGTERM. A second server run with --client-ca then does mutual TLS: a
# client certificate from the trusted CA is admitted and identified, while no
# certificate and one from another CA are refused. Set DIAMOND_BIN to use a diamond other than
# ../../build/diamond, and TLSECHO_PORT to pick the port.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
port="${TLSECHO_PORT:-18200}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2> /dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

cert() { # name common-name [extra req args]
  local name="$1" cn="$2"; shift 2
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$work/$name.key" -out "$work/$name.pem" \
    -days 2 -subj "/CN=$cn" "$@" > /dev/null 2>&1
}
cert server localhost -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
cert stranger stranger
cert clientca "Test Client CA" -addext "basicConstraints=critical,CA:TRUE"
cert evilca "Evil CA" -addext "basicConstraints=critical,CA:TRUE"
client_cert() { # ca name subject
  openssl req -newkey rsa:2048 -nodes -keyout "$work/$2.key" -out "$work/$2.csr" \
    -subj "$3" > /dev/null 2>&1
  openssl x509 -req -in "$work/$2.csr" -CA "$work/$1.pem" -CAkey "$work/$1.key" \
    -CAcreateserial -out "$work/$2.pem" -days 2 > /dev/null 2>&1
}
client_cert clientca alice "/CN=alice/O=Acme"
client_cert evilca mallory "/CN=mallory"

"$diamond" build tlsecho.di -o "$work/tlsecho" > "$work/build.log"
t="$work/tlsecho"
client() { timeout 20 "$t" client "$@"; }
status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

# bash starts background jobs with SIGINT ignored; reset it (docs/networking.md).
(trap - INT TERM; exec "$t" serve "$port" "$work/server.pem" "$work/server.key") > "$work/server.out" 2>&1 &
server_pid=$!
for _ in $(seq 50); do
  grep -q "listening on" "$work/server.out" 2> /dev/null && break
  sleep 0.1
done
grep -q "listening on" "$work/server.out" || { echo "server didn't start" >&2; cat "$work/server.out" >&2; exit 1; }

ca="$work/server.pem"

# The server's own preference wins: it lists tlsecho/2 first.
[[ "$(client localhost "$port" --ca "$ca" --alpn tlsecho/1,tlsecho/2 PROTO | tr '\n' '|')" == "negotiated: tlsecho/2|PROTO -> tlsecho/2|" ]]
# Offering only the older protocol gets it, and GZIP is refused there.
out="$(client localhost "$port" --ca "$ca" --alpn tlsecho/1 PROTO 'GZIP x' | tr '\n' '|')"
[[ "$out" == "negotiated: tlsecho/1|PROTO -> tlsecho/1|GZIP x -> ERR GZIP needs tlsecho/2|" ]]
# Offering nothing negotiates nothing.
[[ "$(client localhost "$port" --ca "$ca" PROTO | tr '\n' '|')" == "negotiated: none|PROTO -> none|" ]]

# Text lines and a length-framed gzip body share one connection.
out="$(client localhost "$port" --ca "$ca" --alpn tlsecho/2 'ECHO hello there' 'GZIP compress me' 'UPPER after the binary frame' BOGUS | tr '\n' '|')"
[[ "$out" == "negotiated: tlsecho/2|ECHO hello there -> hello there|GZIP compress me -> 20 lines of 'compress me', 240 bytes, compressed on the wire: true|UPPER after the binary frame -> AFTER THE BINARY FRAME|BOGUS -> ERR unknown command 'BOGUS'|" ]]

# A second connection resumes the first one's session.
[[ "$(client localhost "$port" --ca "$ca" --resume PROTO | tail -1)" == "resumed: true" ]]

# Verification is not optional. Each refusal is a clean exit 66, and the
# server keeps serving afterwards.
[[ "$(client localhost "$port" --ca "$work/stranger.pem" PROTO 2>&1 || true)" == *"certificate verify failed"* ]]
[[ "$(status_of client localhost "$port" --ca "$work/stranger.pem" PROTO)" == 66 ]]
[[ "$(client 127.0.0.2 "$port" --ca "$ca" PROTO 2>&1 || true)" == *"certificate verify failed"* ]]
[[ "$(client localhost "$port" --ca "$work/missing.pem" PROTO 2>&1 || true)" != "" ]]
[[ "$(status_of client localhost "$port" --ca "$work/missing.pem" PROTO)" == 66 ]]
[[ "$(client localhost "$port" --ca "$ca" PROTO | tail -1)" == "PROTO -> none" ]]

# SIGTERM while blocked in accept: the trapped handler reports and exits 0.
kill -TERM "$server_pid"
status=0; wait "$server_pid" || status=$?; server_pid=""
[[ "$status" == 0 ]]
grep -q "^handshake failed" "$work/server.out"
grep -q "^connection 1: tlsecho/2, 2 requests" "$work/server.out"
grep -q "^stopping after 7 connections" "$work/server.out"

# Mutual TLS: the same service, now demanding a client certificate.
mport=$((port + 1))
(trap - INT TERM; exec "$t" serve "$mport" "$work/server.pem" "$work/server.key" \
  --client-ca "$work/clientca.pem") > "$work/mserver.out" 2>&1 &
server_pid=$!
for _ in $(seq 50); do
  grep -q "listening on" "$work/mserver.out" 2> /dev/null && break
  sleep 0.1
done
grep -q "listening on" "$work/mserver.out" || { echo "mutual-TLS server didn't start" >&2; cat "$work/mserver.out" >&2; exit 1; }

# An admitted client is identified by its certificate's subject (RFC 2253,
# most specific name first).
[[ "$(client localhost "$mport" --ca "$ca" --cert "$work/alice.pem" --key "$work/alice.key" WHOAMI | tail -1)" == "WHOAMI -> O=Acme,CN=alice" ]]
# No certificate, or one from a CA the server doesn't trust, is refused:
# exit 66 with a TLS error, and the server keeps serving afterwards.
[[ "$(status_of client localhost "$mport" --ca "$ca" WHOAMI)" == 66 ]]
[[ "$(client localhost "$mport" --ca "$ca" WHOAMI 2>&1 || true)" == *"TLS"* ]]
[[ "$(status_of client localhost "$mport" --ca "$ca" --cert "$work/mallory.pem" --key "$work/mallory.key" WHOAMI)" == 66 ]]
[[ "$(client localhost "$mport" --ca "$ca" --cert "$work/alice.pem" --key "$work/alice.key" WHOAMI 'ECHO still up' | tail -1)" == "ECHO still up -> still up" ]]
# A certificate without its key (or the reverse) is a usage error.
[[ "$(status_of "$t" client localhost "$mport" --ca "$ca" --cert "$work/alice.pem" WHOAMI)" == 64 ]]

kill -TERM "$server_pid"
status=0; wait "$server_pid" || status=$?; server_pid=""
[[ "$status" == 0 ]]
grep -q "^handshake failed" "$work/mserver.out"
grep -q "^stopping after 2 connections" "$work/mserver.out"

# Without --client-ca a server never asks, so WHOAMI can only say anonymous.
(trap - INT TERM; exec "$t" serve "$mport" "$work/server.pem" "$work/server.key") \
  > "$work/aserver.out" 2>&1 &
server_pid=$!
for _ in $(seq 50); do
  grep -q "listening on" "$work/aserver.out" 2> /dev/null && break
  sleep 0.1
done
[[ "$(client localhost "$mport" --ca "$ca" --cert "$work/alice.pem" --key "$work/alice.key" WHOAMI | tail -1)" == "WHOAMI -> anonymous" ]]
kill -TERM "$server_pid"
wait "$server_pid" || true; server_pid=""

[[ "$(status_of "$t")" == 64 ]]
[[ "$(status_of "$t" serve notaport a b)" == 64 ]]
[[ "$(status_of "$t" client localhost 1 --ca "$ca")" == 64 ]]
[[ "$(status_of "$t" client localhost "$port" PROTO)" == 64 ]]

echo "tlsecho smoke test passed"
