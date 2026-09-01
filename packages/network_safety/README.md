# network_safety

A basic SSRF (server-side request forgery) guard for [Diamond](https://gitlab.com/dmn9180/diamond)'s
outbound HTTP client (`packages/http`) — ported from `ModFederate::NetworkSafety`
(MaquinasStack's private Ruby gem). Built for an ingest-style caller that
hands a URL from a third-party API's own response (not raw end-user
input) to `http_get`/`http_post` — a skin-download link from a GraphQL
response, say — and wants a cheap check against an obviously-local
target before doing so.

## Install

Same story as every other package here — copy this directory into
another project as `cuts/network_safety/`, or give it its own git remote
and depend on it via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Usage

```ruby
require "/path/to/network_safety/lib/network_safety"

parsed = resolve_public_hostname("https://api.example.com/data")
# => {"scheme": "https", "host": "api.example.com", "port": 443,
#     "path": "/data", "default_port": 443}
# -- the same shape packages/http's own http_parse_url returns, since
# that's exactly what this wraps.

resolve_public_hostname("http://127.0.0.1/x")
# => raises ArgumentError: "URL host is a private/reserved IPv4 address: 127.0.0.1"
```

Call it before `http_get`/`http_post`, then make the request with the
original URL string as usual (this doesn't change how you call the HTTP
client, it's a check you run first):

```ruby
def fetch(url)
  resolve_public_hostname(url)  # raises ArgumentError if rejected
  http_get(url)
end
```

## What's checked, and what isn't

`resolve_public_hostname(url)` raises `ArgumentError` (never a silent
`nil`) for:

- An unsupported or missing scheme (delegated to `http_parse_url`'s own
  check — only `http`/`https` are accepted).
- Embedded userinfo (`http://user:pass@host/...`).
- A literal IPv6 host (`http://[::1]/...`) — rejected outright, not
  parsed (see "Diamond-specific notes" below for why).
- A missing host.
- A host that's a private/loopback/link-local/reserved **IPv4 literal**
  address — the same 14 ranges `ModFederate::NetworkSafety`'s own
  `BLOCKED_NETWORKS` covers for IPv4 (`0.0.0.0/8`, `10.0.0.0/8`,
  `100.64.0.0/10`, `127.0.0.0/8`, `169.254.0.0/16`, `172.16.0.0/12`,
  `192.0.0.0/24`, `192.0.2.0/24`, `192.168.0.0/16`, `198.18.0.0/15`,
  `198.51.100.0/24`, `203.0.113.0/24`, `224.0.0.0/4`, `240.0.0.0/4`).
- `"localhost"` or a hostname ending in `.local`/`.internal`.

**What it does *not* catch: a hostname that *resolves* to a private
address.** `evil.example.com` pointed at `127.0.0.1` in DNS passes this
check — only a URL that's *literally* an IP address in one of the
blocked ranges (or an obviously-local name) is rejected. This is a real,
deliberate reduction from the Ruby original, explained fully below.

## Diamond-specific notes

- **String-level checks only — Diamond genuinely cannot do better right
  now, not a design preference.** The Ruby original resolves the
  hostname via DNS, validates the *resulting IP address*, then connects
  to that exact IP (bypassing a second lookup) — so a hostname that
  resolves to a private address is caught, and a DNS answer that changes
  between the check and the connect (DNS rebinding) can't slip through
  either. Diamond has no way to resolve a hostname to an IP address
  without actually connecting — `TCPSocket.connect` does resolve+connect
  as one step and never exposes the resolved address afterward — so
  neither protection is buildable at the Diamond-language level today.
  A native `Socket.resolve`-style VM primitive (`getaddrinfo` without
  connecting) would close this gap properly; that's a separate, bigger
  change (touches `src/vm.c`, needs a rebuild), not attempted here.
- **IPv4 only.** Diamond has no bitwise-AND operator (only `<<`, for
  left-shift) and no native `IPAddr` class, so CIDR containment is done
  via integer division instead: two addresses share a `/prefix` network
  iff they're equal after dividing out the `2^(32-prefix)` host-bit
  range (`network_safety_ipv4_blocked?`) — exact, no masking needed.
  That trick doesn't scale as cleanly to IPv6's 128-bit address space,
  and the APIs this guards are plain IPv4/dual-stack HTTPS endpoints, so
  an IPv6 blocklist table isn't attempted — a literal IPv6 host is
  rejected outright instead of evaluated.
- **A literal IPv6 host has to be caught *before* `http_parse_url` runs,
  not after.** `http_parse_url` has no bracket-notation (`[::1]`)
  support and splits on the first `:` it finds — `[::1]` alone parses
  into host `[`, port `0` (`"::1]".to_i()`), silently losing the real
  host string entirely. `resolve_public_hostname` extracts the raw
  `host[:port]` segment itself first (`network_safety_host_and_port_segment`)
  and checks it for a leading `[` (and independently, for `@`, the
  userinfo case) before ever calling `http_parse_url` — confirmed
  directly: checking `parsed["host"]` *after* the call, as a first
  attempt did, let `http://[::1]/x` straight through, since the mangled
  host `"["` contains no `:` for a naive after-the-fact check to catch.

## What's deliberately out of scope

- **DNS-level resolution/validation.** See "Diamond-specific notes"
  above — not a scope cut so much as a real, current language
  limitation.
- **IPv6 blocked-network checking.** See above.
- **Non-HTTP(S) schemes.** Delegated entirely to `http_parse_url`.

## Test

```sh
make test-network-safety-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
