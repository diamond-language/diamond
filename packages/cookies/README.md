# packages/cookies

Cookie parsing/serialization plus signed and encrypted cookie support for
[Diamond](https://gitlab.com/dmn9180/diamond), built on the native
`Cipher.encrypt`/`.decrypt` (AES-256-GCM) and `HMAC.sha256`/`.verify`
builtins (`docs/syntax.md`). Depended on by
[`packages/rack`](../rack/README.md) for its `CookieSession` middleware,
but usable directly by any `http_serve`/`gremlin_serve` app that doesn't
use `rack` at all — this package has no dependency on either server, or
on `rack` itself.

## Install

Same story as `packages/http`/`packages/rack` — copy this directory into
another project as `cuts/cookies/`, or give it its own git remote and
depend on it via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Cookie parsing and serialization

```ruby
require "/path/to/cookies/lib/cookies"

cookie_parse(request["headers"]["cookie"])
# => {"session_id": "abc123", "theme": "dark"}

cookie_serialize("session_id", "abc123", {"path": "/", "max_age": 3600})
# => "session_id=abc123; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax"
```

`cookie_parse(header)` splits a `Cookie:` request header into a `Hash`.
Last write wins on a duplicate name. No percent-decoding of values —
every value this package itself produces is `base64url` (already
cookie-safe, nothing to unescape); decoding a third-party cookie's own
percent-encoded value is out of scope.

`cookie_serialize(name, value, options = {})` builds one `Set-Cookie`
header *value* (not the whole header line — see "Multiple cookies in one
response" below for why that distinction matters). `options`: `path`,
`domain`, `max_age`, `expires` (only added if given); `http_only`
(default `true`); `same_site` (default `"Lax"`); `secure` (default
`false`, left for the caller to opt into explicitly — defaulting it on
would silently break a plain `http_serve` local-dev setup with no TLS).

## Multiple cookies in one response

`packages/http`'s `http_write_response` writes one `Set-Cookie:` line per
`Array` element when a response header's value is an `Array`, instead of
one line for a whole `String` value — needed because `Set-Cookie` can't
be comma-joined into one line the way most repeated headers can (its own
`Expires=...` attribute already contains a comma). Set two cookies in one
response like this:

```ruby
[200, {"Set-Cookie": [
  cookie_serialize("a", "1"),
  cookie_serialize("b", "2"),
]}, body]
```

## `base64url_encode`/`base64url_decode`, `hex_decode`

Pure Diamond, no VM changes — `base64url_encode(bytes)`/
`base64url_decode(text)` implement RFC 4648's URL-safe alphabet with no
`=` padding (like a JWT), and `hex_decode(hex)` is the inverse of
`Digest.sha256`'s own hex output. Both return `nil` (not an exception) for
malformed input. Built on integer division/`mod` rather than bit-shifting
or masking — Diamond has no `>>`/bitwise-`&` (only `<<`, for `Int`
left-shift and `Array` push — see `docs/syntax.md`), which works cleanly
here since every intermediate value is a non-negative byte or 6-bit
group.

## `SignedCookies` — tamper-evident, not confidential

```ruby
signed = SignedCookies.sign("user:42", "topsecret")
SignedCookies.verify(signed, "topsecret")   # => "user:42"
SignedCookies.verify(signed, "wrong")       # => nil
```

`base64url(value) + "." + HMAC-SHA256(secret, value)`. `.verify` uses the
native `HMAC.verify`'s constant-time comparison, not a plain `==`, to
avoid a timing side-channel on the signature check. Good for values where
readability doesn't matter and confidentiality isn't the point — a CSRF
token, a plain user id.

## `EncryptedCookies` — confidential and tamper-evident

```ruby
enc = EncryptedCookies.encrypt("sensitive session data", "topsecret")
EncryptedCookies.decrypt(enc, "topsecret")   # => "sensitive session data"
EncryptedCookies.decrypt(enc, "wrong")       # => nil
```

Built on `Cipher.encrypt`/`.decrypt` (AES-256-GCM) — its own
authentication tag check is the tamper-detection, so there's no separate
signature step and no extra timing side-channel to worry about; the
whole check happens inside OpenSSL. `secret` can be any length: SHA-256
of it derives the raw 32-byte key `Cipher` itself requires. This is
`SHA-256` of a secret, **not a real KDF** (PBKDF2/HKDF) — adequate for
deriving one AES key from one long-lived app secret, not a substitute for
one if this ever needs to derive several independent keys from the same
secret.

## `CookieSession` — the Rack middleware

A `Callable[3]` `(request, context, forward)` matching
[`packages/rack`](../rack/README.md)'s own middleware contract: reads a
named cookie (default `"_session"`), decrypts and `JSON.parse`s it into
`request["session"]` (a plain mutable `Hash` — starts at `{}` if the
cookie is missing, tampered, or signed/encrypted with a different secret,
rather than raising) before calling `forward`; after `forward` returns,
`JSON.stringify`s the (possibly mutated) session `Hash`, encrypts it, and
appends it onto the response's `Set-Cookie` header (preserving any cookie
the app handler already set itself).

Must be **configured once per `DiamondVm`** before use, via `.configure`
(a class-variable write) — and per `packages/rack`'s own `RackChain`
section, every `Thread.new`-spawned `gremlin_serve` worker already gets
its own fully independent `DiamondVm`, so `.configure` needs calling
inside whatever function already runs once per worker (the same
`build_chain()`-style function `RackChain`'s own pattern already calls
per worker), not just once at the top of the script before threads are
spawned:

```ruby
require "/path/to/gremlin/lib/gremlin"
require "/path/to/rack/lib/rack"
require "/path/to/cookies/lib/cookies"

def app_handler(request, context)
  visits = request["session"]["visits"]
  visits = if visits == nil then 0 else visits end
  request["session"]["visits"] = visits + 1
  [200, {"Content-Type": "text/plain"}, "visit ##{visits}"]
end

def build_chain()
  CookieSession.configure(secret: ENV["SESSION_SECRET"])
  rack_compose([CookieSession.call, Csrf.call], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve(8080, rack_app, threads: 4)
```

At `threads: 1` (or with `http_serve`, which is single-threaded and has
no `Thread.new` involved at all), calling `.configure` once at the top
level works too — see `packages/rack`'s own README for the full story on
why `RackChain`/the `build_chain()` pattern exists in the first place.

## `Csrf` — CSRF protection

A `Callable[3]` middleware for the synchronizer-token pattern, built on
`CookieSession`'s `request["session"]` — it must run **after**
`CookieSession` in the chain (`rack_compose([CookieSession.call,
Csrf.call], app_handler)`, as in the example above), the same ordering
requirement any session-dependent middleware has.

`Csrf.token(request)` lazily mints one 256-bit token
(`SecureRandom.hex(32)`) per session and stores it there, so it's stable
across requests the same way the rest of the session is — call it from
your own handler to get the value to embed in a form's hidden field, or
hand to client-side JS to echo back as a request header:

```ruby
def form_handler(request, context)
  [200, {"Content-Type": "text/html"},
    "<input type=\"hidden\" name=\"csrf_token\" value=\"#{Csrf.token(request)}\">"]
end
```

`Csrf.call` ensures a token exists on every request (so even the first
`GET` that renders a form already has one), skips the check entirely for
`GET`/`HEAD`/`OPTIONS` (methods that must never carry a state-changing
side effect), and otherwise requires the request's own `X-CSRF-Token`
header to match the session's token — compared via `constant_time_equal`
(below), not a plain `==`, to avoid a timing side-channel on the token
itself — rejecting with `403` if it's missing or doesn't match:

```ruby
Csrf.call({"method": "POST", "session": {"csrf_token": "abc"},
  "headers": {"x-csrf-token": "abc"}}, {}, app_handler)   # => calls app_handler
Csrf.call({"method": "POST", "session": {"csrf_token": "abc"},
  "headers": {}}, {}, app_handler)                        # => [403, ..., "invalid or missing CSRF token"]
```

Checking a request header rather than parsing a form body is deliberate
— this codebase has no built-in form-body parser to hook into (see
`packages/http`'s own README), and a header is also what an XHR/`fetch`-
based client sends most naturally. A traditional HTML form submission
needs its own tiny client-side script to copy the hidden field's value
into the `X-CSRF-Token` header before submitting.

`constant_time_equal(a, b)` (used by `Csrf.valid?` above) compares two
`String`s in constant time without a new VM primitive: `HMAC(k, a) ==
HMAC(k, b)` iff `a == b`, so it reuses the native `HMAC.sha256`/
`HMAC.verify` (`docs/syntax.md`) with a fixed, non-secret key purely to
get their constant-time comparison for free.

## What's deliberately out of scope

- **Cookie signing/verification for arbitrary structured data beyond one
  string.** `SignedCookies`/`EncryptedCookies` work on `String`s; encode
  a `Hash`/`Array` to `JSON.stringify` first if you need one, the way
  `CookieSession` itself does for the session `Hash`.
- **A real key-derivation function.** `EncryptedCookies`' `secret` ->
  AES-key derivation is a single `SHA-256`, not PBKDF2/HKDF — fine for one
  key from one app secret, not a multi-key KDF.
- **Percent-decoding third-party cookie values.** See "Cookie parsing and
  serialization" above.
- **Parsing a CSRF token out of a submitted form body.** `Csrf` only
  checks the `X-CSRF-Token` request header — see `Csrf`'s own section
  above.
- Everything `packages/rack`'s own README already lists as out of scope
  (routing) applies here too.
