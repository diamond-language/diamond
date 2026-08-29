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
# (EncryptedCookies) to turn Digest.sha256(secret)'s hex output into a
# raw AES key; expects lowercase hex, matching what Digest.sha256/
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

# Compares two Strings in constant time, for cases (Csrf) that need it
# but aren't themselves an HMAC check the native HMAC.verify already
# covers. Built on HMAC.verify/HMAC.sha256 rather than a new VM
# primitive: HMAC(k, a) == HMAC(k, b) iff a == b (HMAC is a keyed
# pseudorandom function -- a collision here is as infeasible as forging
# a signature), and HMAC.verify's own comparison of the two digests is
# already CRYPTO_memcmp underneath. `key` only has to be *consistent*
# between the two calls, not secret -- it's not standing in for a real
# HMAC key here, just reusing the primitive's own constant-time compare.
def constant_time_equal(a, b)
  key = "cookies.constant_time_equal"
  HMAC.verify(a, key, HMAC.sha256(key, b))
end
