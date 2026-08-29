# Cookie parsing/serialization, base64url/hex helpers, and signed/
# encrypted cookie support -- built on top of Cipher.encrypt/.decrypt
# and HMAC.sha256/.verify (src/vm.c), the same OpenSSL-backed native
# builtins BCrypt/Digest/SecureRandom already are. See README.md for
# the full story and packages/rack for the middleware contract this is
# meant to plug into.

# Splits a `Cookie:` request header ("a=1; b=2") into {"a": "1", "b":
# "2"}. Last-write-wins on a duplicate name (an ordinary Hash write, no
# special casing) -- matches typical browser/cookie-jar semantics.
# No percent-decoding: every value this package itself produces is
# base64url (already cookie-safe, nothing to unescape); decoding a
# third-party cookie's percent-encoded value is out of scope here.
def cookie_parse(header)
  cookies = {}
  if header == nil || header == ""
    return cookies
  end
  def store_pair(raw)
    pair = raw.strip()
    equals = pair.index_of("=")
    if equals != nil
      name = pair.slice(0, equals)
      value = pair.slice(equals + 1, pair.length())
      cookies[name] = value
    end
  end
  header.split(";").each(store_pair)
  cookies
end

# Builds one Set-Cookie header *value* (not the whole header line --
# matches the "one Hash value == one header value" convention
# packages/http's own http_write_response already uses). `options`:
# "path", "domain", "max_age", "expires" (only added if given),
# "http_only" (default true), "same_site" (default "Lax"), "secure"
# (default false -- left for the caller to opt into explicitly, since
# defaulting it on would silently break a plain http_serve local dev
# setup with no TLS).
def cookie_serialize(name, value, options = {})
  parts = ["#{name}=#{value}"]
  path = options["path"]
  unless path == nil
    parts.push("Path=#{path}")
  end
  domain = options["domain"]
  unless domain == nil
    parts.push("Domain=#{domain}")
  end
  max_age = options["max_age"]
  unless max_age == nil
    parts.push("Max-Age=#{max_age}")
  end
  expires = options["expires"]
  unless expires == nil
    parts.push("Expires=#{expires}")
  end
  http_only = options["http_only"]
  if http_only == nil || http_only
    parts.push("HttpOnly")
  end
  if options["secure"] == true
    parts.push("Secure")
  end
  same_site = options["same_site"]
  same_site = if same_site == nil then "Lax" else same_site end
  parts.push("SameSite=#{same_site}")
  parts.join("; ")
end

# base64url (RFC 4648 URL-safe alphabet, no '=' padding -- every group
# of raw bytes decodes back to its exact original length without
# needing padding to know where the input ends, the same reasoning
# JWTs skip padding for) encode/decode. Pure Diamond -- no VM changes --
# built on integer division/mod instead of bit-shifting/masking (Diamond
# has no `>>`/bitwise-`&`; only `<<` exists, for Int left-shift and Array
# push -- see docs/syntax.md), which works cleanly here since every value
# is a non-negative byte or 6-bit group. Needed because Cipher's binary
# blob isn't cookie-safe as raw bytes (cookies can't contain arbitrary
# control characters, ';', ',', or spaces).
def base64url_encode(bytes)
  alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
  chars = []
  length = bytes.length()
  index = 0
  while index < length
    remaining = length - index
    b0 = bytes.slice(index, 1).ord()
    chars.push(alphabet.slice(b0 / 4, 1))
    if remaining == 1
      chars.push(alphabet.slice(mod(b0, 4) * 16, 1))
    else
      b1 = bytes.slice(index + 1, 1).ord()
      chars.push(alphabet.slice(mod(b0, 4) * 16 + b1 / 16, 1))
      if remaining == 2
        chars.push(alphabet.slice(mod(b1, 16) * 4, 1))
      else
        b2 = bytes.slice(index + 2, 1).ord()
        chars.push(alphabet.slice(mod(b1, 16) * 4 + b2 / 64, 1))
        chars.push(alphabet.slice(mod(b2, 64), 1))
      end
    end
    index = index + 3
  end
  chars.join("")
end

# Inverse of base64url_encode. Returns nil (not an exception) for
# malformed input -- a single leftover character that can't encode a
# whole byte, or any character outside the alphabet -- matching how
# Cipher.decrypt/HMAC.verify already treat "bad input" as a normal
# outcome rather than a programmer error.
def base64url_decode(text)
  alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
  bytes = []
  length = text.length()
  index = 0
  while index < length
    remaining = length - index
    if remaining == 1
      return nil
    end
    c0 = alphabet.index_of(text.slice(index, 1))
    c1 = alphabet.index_of(text.slice(index + 1, 1))
    if c0 == nil || c1 == nil
      return nil
    end
    bytes.push(chr(c0 * 4 + c1 / 16))
    if remaining >= 3
      c2 = alphabet.index_of(text.slice(index + 2, 1))
      if c2 == nil
        return nil
      end
      bytes.push(chr(mod(c1, 16) * 16 + c2 / 4))
      if remaining >= 4
        c3 = alphabet.index_of(text.slice(index + 3, 1))
        if c3 == nil
          return nil
        end
        bytes.push(chr(mod(c2, 4) * 64 + c3))
      end
    end
    index = index + 4
  end
  bytes.join("")
end

# Pairs of lowercase hex digits -> raw bytes -- pure Diamond, same
# integer-arithmetic technique as base64url above. Only used internally
# (EncryptedCookies below) to turn Digest.sha256(secret)'s hex output
# into a raw AES key; expects lowercase hex, matching what Digest.sha256/
# HMAC.sha256 always produce (this codebase never emits uppercase hex).
# Returns nil for an odd-length input or a non-hex character.
def hex_decode(hex)
  digits = "0123456789abcdef"
  bytes = []
  length = hex.length()
  if mod(length, 2) != 0
    return nil
  end
  index = 0
  while index < length
    high = digits.index_of(hex.slice(index, 1))
    low = digits.index_of(hex.slice(index + 1, 1))
    if high == nil || low == nil
      return nil
    end
    bytes.push(chr(high * 16 + low))
    index = index + 2
  end
  bytes.join("")
end

# Tamper-evident, not confidential -- for cases like a CSRF token or a
# plain user id where readability doesn't matter. base64url(value) + "."
# + its HMAC-SHA256 hex signature; .verify uses HMAC.verify's constant-
# time comparison rather than plain `==`, matching how BCrypt.verify
# already avoids a timing side-channel on its own comparison.
class SignedCookies
  def self.sign(value: String, secret: String) -> String
    "#{base64url_encode(value)}.#{HMAC.sha256(secret, value)}"
  end

  def self.verify(cookie_value: String, secret: String)
    dot = cookie_value.index_of(".")
    if dot == nil
      return nil
    end
    encoded = cookie_value.slice(0, dot)
    signature = cookie_value.slice(dot + 1, cookie_value.length())
    value = base64url_decode(encoded)
    if value == nil
      return nil
    end
    if HMAC.verify(value, secret, signature)
      value
    else
      nil
    end
  end
end

# Confidential and tamper-evident in one step, via Cipher's AES-256-GCM
# (its own authentication tag check does the tamper-detection -- no
# separate signature needed, unlike SignedCookies above, and no timing
# side-channel exposed to Diamond code either, since the whole check
# happens inside OpenSSL's EVP_DecryptFinal_ex). `secret` can be any
# length -- SHA-256 of it derives the raw 32-byte AES key Cipher itself
# requires. This is SHA-256 of an arbitrary-length secret, not a real
# KDF (PBKDF2/HKDF) -- adequate for deriving one key from one long-lived
# app secret, not a substitute for one if this ever needs to derive
# multiple independent keys from the same secret.
class EncryptedCookies
  def self.key_from_secret(secret: String) -> String
    hex_decode(Digest.sha256(secret))
  end

  def self.encrypt(value: String, secret: String) -> String
    base64url_encode(Cipher.encrypt(EncryptedCookies.key_from_secret(secret), value))
  end

  def self.decrypt(cookie_value: String, secret: String)
    blob = base64url_decode(cookie_value)
    if blob == nil
      return nil
    end
    Cipher.decrypt(EncryptedCookies.key_from_secret(secret), blob)
  end
end

# Appends `value` onto whatever `existing` already holds for a
# response's Set-Cookie entry -- nil (nothing set yet), a single
# String (one cookie already set), or an Array (several already set) --
# returning the Array to store back. Kept as its own top-level function
# rather than nested inside CookieSession.call: a nested `def` can only
# close over a handful of outer locals (DIAMOND_MAX_BOUND_VALUES, see
# src/vm.c), and .call already has too many in scope by this point.
def cookie_session_merge_header(existing, value)
  merged = []
  unless existing == nil
    case existing
    when [*lines]
      def collect_line(line)
        merged.push(line)
      end
      lines.each(collect_line)
    else
      merged.push(existing)
    end
  end
  merged.push(value)
  merged
end

# The actual Rack cookie-session middleware: reads a named cookie,
# decrypts+JSON-parses it into request["session"] (a plain mutable Hash,
# {} if missing/tampered/wrong secret) before calling `forward`; after
# `forward` returns, re-serializes the (possibly mutated) session Hash
# back into a Set-Cookie response header.
#
# Must be configured once per VM before use -- `.configure` sets class
# variables, and (see packages/rack's own README on RackChain) each
# Thread.new-spawned gremlin_serve worker already gets its own fully
# independent DiamondVm, so `.configure` needs calling inside whatever
# function already runs once per worker (the same `build_chain()`-style
# function RackChain's own pattern already calls per worker), not just
# once at the top of the script before threads are spawned:
#
#   def build_chain()
#     CookieSession.configure(secret: ENV["SESSION_SECRET"])
#     rack_compose([CookieSession], app_handler)
#   end
#
#   def rack_app(request, context)
#     rack_run_chain(RackChain.get(build_chain), 0, request, context)
#   end
class CookieSession
  def self.configure(secret: String, cookie_name: String = "_session")
    @@secret = secret
    @@cookie_name = cookie_name
  end

  def self.call(request, context, forward)
    cookie_name = @@cookie_name
    cookies = cookie_parse(request["headers"]["cookie"])
    raw = cookies[cookie_name]
    session = {}
    unless raw == nil
      decrypted = EncryptedCookies.decrypt(raw, @@secret)
      unless decrypted == nil
        begin
          session = JSON.parse(decrypted)
        rescue error: JSONError
          session = {}
        end
      end
    end
    request["session"] = session
    response = forward(request, context)
    [status, headers, body] = response
    encoded = EncryptedCookies.encrypt(JSON.stringify(session), @@secret)
    set_cookie_value = cookie_serialize(cookie_name, encoded, {})
    merged = cookie_session_merge_header(headers["Set-Cookie"], set_cookie_value)
    [status, headers.merge({"Set-Cookie": merged}), body]
  end
end
