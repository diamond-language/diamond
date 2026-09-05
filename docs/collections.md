# Collections and Enumerable

[Language reference](syntax.md) · Previous: [Gradual typing and exceptions](types-and-errors.md) · Next: [Runtime features and debugging](runtime-reference.md)

`[1, 2]` for arrays and `{"key": value}` for hashes, both with optional
type parameters, and both can be split across lines — a newline is
allowed right after the opening bracket, right after each `,`, and
right before the closing bracket, the same as every other
bracket-delimited, comma-separated list in the language (call
arguments, `def` parameter declarations, generic type-argument lists,
and more):

```ruby
config = {
  "name": "myapp",
  "values": [
    1,
    2,
  ],
}
```

`.push`, `.pop`, and `.length()` are native on both. Array additionally
has `.join(separator = "")`, native and O(n) total (a `StringBuilder`-
backed accumulator internally, not repeated string concatenation) —
stringifies each element (the same formatting string interpolation
uses, including calling a user-defined `to_s` override) and joins them
with `separator` between (not trailing).

Array's Enumerable-style methods: `.each`/`.select`/`.map`/`.reduce`/
`.count`/`.any?`/`.all?` (shared with Hash, driven by `.each`),
`.sort`/`.sort_by`/`.min`/`.max`/`.min_by`/`.max_by`/`.reject`/`.find`/
`.each_with_index`/`.sum`, and `.take(n)`/`.drop(n)`/`.flat_map`/
`.partition`/`.group_by`/`.zip(other)`/`.each_slice(n)`/`.each_cons(n)`/
`.tally`. Calling `.lazy()` on an Array, Range, or other Enumerable
returns a `LazyEnumerator`. Its `map`, `select`, and `reject` operations are
composable and deferred until a terminal runs; chained transforms do not
allocate intermediate Arrays. `each`, `to_a`, and `force` drain the pipeline.
`take`, `find`, `any?`, and `all?` short-circuit through the cooperative
`each_until` protocol; Arrays, Hash values, and Ranges stop pulling as soon as
the terminal has its answer. The remaining methods retain their eager
behavior. The last group returns its result directly —
`.each_slice`/`.each_cons` collect every slice/window into an
`Array[Array]` up front, `.partition` returns `[matching,
non_matching]`, `.group_by` a `Hash` keyed on the block's own result,
`.tally` a `Hash` of element to occurrence count, and `.zip` pads the
shorter array with `Nil` out to the *receiver's* own length (`[1, 2,
3].zip([4, 5])` => `[[1, 4], [2, 5], [3, nil]]`), matching Ruby. None of
these are defined on `Hash` — a deliberate, narrower scope than Ruby's
own Enumerable, matching the existing asymmetry `.min`/`.max`/`.sort`
already have (Array-only, not Hash).
Strings support `.length()`, `.index_of(needle)` (position or `nil`),
`.slice(start, length)`, `.to_i()`/`.to_f()` (lenient decimal parsing —
`.to_f()` additionally accepts exponent notation like `"1e3"` even
though Diamond's own float literals don't, and overflows to `Infinity`
rather than raising, unlike `.to_i()`; neither skips leading
whitespace, and `"nan"`/`"inf"` parse as `0.0`, matching Ruby's
`String#to_f`), and `.downcase()`/`.upcase()` (ASCII-only case
conversion), and
`.reverse()` (byte-level, not UTF-8-grapheme-aware — consistent with
the rest of the language having no other Unicode awareness), and
`.strip()` (trims leading/trailing ASCII whitespace). `.split(separator)`
returns an `Array` of every piece around non-overlapping occurrences of
`separator` (an empty `separator` splits into one-character strings);
unlike Ruby, it keeps every piece including empty ones from consecutive
or leading/trailing separators (no trailing-empty suppression) — a
deliberate simplification, not an attempt at Ruby compatibility.
`.ord()` returns the first byte's value as an `Int`; an empty String
raises a rescuable `IndexError`. `chr(code)` is its inverse — a global
function (not receiver syntax; `Int` has no per-value method dispatch)
returning a one-character `String`, recognized the same way `gets()`
is (shadowable by a local or top-level function). `code` outside
`0..255` raises a rescuable `RangeError`.

`.format(values)` is a `sprintf`-style formatter — `values` is either a
single value or an `Array` of them (matching Ruby's `String#%`, without
needing variadic/splat call support Diamond doesn't have):

```ruby
"Name: %s, Age: %d".format(["Alice", 30])  # => "Name: Alice, Age: 30"
"%05d".format(42)                          # => "00042"
"%-10s|".format("hi")                      # => "hi        |"
"%.2f".format(3.14159)                     # => "3.14"
```

Directives: `%d`/`%i` (`Int` or `Float`, truncated), `%f` (`Float` or
`Int`, default 6 decimal places, `%.Nf` for `N`), `%x`/`%X`/`%o`/`%b`
(`Int`, hex/octal/binary — `%b` has no C `printf` equivalent, hand-
rolled), `%s` (any value, via the same formatting string interpolation
uses, including a user-defined `to_s` override), and `%%` for a literal
`%`. `-` left-justifies and `0` zero-pads within a numeric width prefix
(`%-10s`, `%05d`); `.N` sets `%f`'s precision. Every directive's
argument type is checked against what that directive actually needs
(`TypeError` on a mismatch); too few arguments raises `ArgumentError`,
extra arguments are silently ignored. Each directive is handled by
building a small, internally-chosen conversion string from the parsed
flags/width/precision and handing it to a real `snprintf` alongside
exactly one correctly-typed value — never your format string forwarded
into a C varargs call directly, which would be a real format-string
vulnerability given a Diamond value's runtime type has no fixed
relationship to what a positionally-matched C conversion expects.

Strings also support `[]` with a single `Int` index, returning a new
one-character `String` (bounds-checked, `IndexError` outside the
string — the same as `.slice()`); unlike Array/Hash, `[]=` on a String
is rejected outright with a `TypeError` (strings are immutable).
`.repeat(n)` returns a new `String` with the receiver repeated `n`
times (`n == 0` → `""`); a negative `n` raises a rescuable `RangeError`.
This is deliberately a method, not `*` — `"x" * 3` isn't supported,
since making the `*` operator polymorphic over String would need a
new deoptimization mechanism for the compiler's Int-only fast path
(`MULTIPLY_INT`) that doesn't otherwise exist for it.

`.start_with?(prefix)`/`.end_with?(suffix)` are plain literal String
checks (no `Regexp` support). `.ljust(width, padding)`/`.rjust(width,
padding)` pad the receiver on the right/left with `padding` (repeated
and truncated as needed) until it reaches `width`, or return the
receiver unchanged if it's already at least `width` long; a negative
`width` or an empty `padding` both raise a rescuable `ArgumentError`
rather than looping forever. `.tr(from, to)` is Ruby-style
character-set translation: `from`/`to` support `a-z`-style ranges and a
leading `^` to negate `from`, with `\` escaping a literal `-` or `^`;
each character in the receiver found in `from` is replaced by the
character at the same position in `to` (a shorter `to` maps every
remaining `from` character onto its own last character, matching
Ruby), or deleted outright when `to` is `""`. An empty `from` raises
`ArgumentError`; a non-`String` argument to any of these raises
`TypeError`.

```ruby
"hello".start_with?("he")          # => true
"abc".ljust(6, "-")                # => "abc---"
"abc".rjust(6, "-")                # => "---abc"
"hello".tr("el", "ip")             # => "hippo"
"hello".tr("aeiou", "*")           # => "h*ll*"
```

`.sub(pattern, replacement)`/`.gsub(pattern, replacement)` replace the
first/every match of `pattern` (a `Regexp` -- a `String` pattern raises
`TypeError`, there's no implicit `Regexp.new` coercion) with
`replacement`, which may contain Ruby-style backreferences (`\0` the
whole match, `\1`.. a capture group; a literal backslash is `\\`).
`.scan(pattern)` (`Regexp` only, same `TypeError` on a `String`) returns
an `Array` of every match: the matched `String` itself when `pattern`
has no capture groups, or an `Array` of that match's captures when it
does -- never a mix of both across one call, since a given `Regexp`
either has groups or doesn't:

```ruby
"hello world".gsub(Regexp.new("o"), "0")                  # => "hell0 w0rld"
"2024-01-15".gsub(Regexp.new("(\\d+)-(\\d+)-(\\d+)"), "\\3/\\2/\\1")  # => "15/01/2024"
"a1 b22 c333".scan(Regexp.new("[0-9]+"))                   # => ["1", "22", "333"]
"key1=val1;key2=val2".scan(Regexp.new("([a-z0-9]+)=([a-z0-9]+)"))
# => [["key1", "val1"], ["key2", "val2"]]
```

See the [Regexp guide](runtime-reference.md#regexp) for pattern/option syntax.

`.each(callback)`, and the `Enumerable` methods derived from it —
`.select`/`.count`/`.any?`/`.all?`/`.reduce`/`.map` — work as receiver
syntax on both arrays and hashes (a hash's Enumerable operates over
*values*, discarding keys), forwarding at runtime to ordinary Diamond
functions in the prelude:

```ruby
[1, 2, 3, 4].select(is_even)
{"a": 1, "b": 2}.count(is_positive)
[1, 2, 3].reduce(0, add)
```

Native collections are extensible from Diamond source without VM changes.
Defining `array_name(values, ...)`, `hash_name(values, ...)`, or the shared
fallback `enumerable_name(values, ...)` exposes `.name(...)` on the matching
receiver. A trailing predicate `?` is omitted from the bridge function name,
so `array_large(values, minimum)` implements `values.large?(minimum)`.
Built-in VM operations take precedence over extension bridges.

Any user-defined class gets those same six methods, plus `to_a`, `sort`,
`sort_by`, `min`, `max`, `min_by`, `max_by`, `reject`, `find`,
`each_with_index`, `sum`, `take`, `drop`, `flat_map`, `partition`,
`group_by`, `zip`, `each_slice`, `each_cons`, and `tally`, for free by
implementing its own `each(callback)` and `include`-ing `Enumerable`. The
first six forward straight to prelude functions driven by `self.each(...)`,
so they're generic by construction; the rest reuse Array's own existing,
already-tested index-based implementations by materializing the receiver
into an Array via `to_a` first (itself built on `each`) and delegating to
those, rather than re-deriving each one generically over `each()`.

A handful of further Array/Hash conveniences work as receiver syntax too,
forwarding the same way the Enumerable set above does:
`values.first()`/`values.last()` return the first/last element (an empty
array's `IndexError` propagates straight from the underlying `[]`);
`values.first_or(fallback)`/`values.last_or(fallback)` are the safe form,
returning `fallback` instead when `values` is empty; `values.empty?()`
returns whether the array has zero elements; `values.include?(needle)`
returns whether any element `==` `needle`; `values.reverse()` returns a new
array in reverse order; `values.concat(other)` returns a new array with
`other`'s elements appended; `values.compact()` returns a new array with
any `nil` elements dropped; `values.uniq()` returns a new array with only
the first occurrence of each distinct (`==`) element, order preserved;
`values.flatten()` returns a new array with nested arrays fully
flattened (recursively, matching Ruby's default);
`values.delete_at(index)` mutates `values` in place (like the native
`.push`/`.pop`), removing and returning the element at `index`, or `nil`
without mutating if `index` is out of bounds.

`hash.fetch(key, fallback)` returns the value at `key`, or `fallback`
(not raising) when `key` is absent; `hash.keys()`/`hash.values()` return
an `Array` of the hash's keys/values respectively, both in insertion
order; `hash.include_key?(needle)` returns whether `needle` is a key;
`hash.map_values(callback)` returns a new `Hash` with the same keys and
each value passed through `callback`; `hash.empty?()` returns whether the
hash has zero pairs (same name and meaning as Array's own); `hash.merge(other)`
returns a new `Hash` with the receiver's pairs then `other`'s applied on top
(`other` wins on key conflicts). Like the rest of the Enumerable set,
`vm.c`'s native dispatch resolves each of these receiver calls to a
same-named top-level prelude function (`array_reverse`, `array_concat`,
...) at runtime, so that free-function spelling still exists underneath
and can't be removed without a native-dispatch rework -- receiver syntax
is simply the only spelling documented and used going forward.
`array_join(values, separator = "")` was the one exception: a thin
wrapper *around* the already-native `.join()` above rather than
`.join()`'s own implementation, with no dispatch dependency on its name,
so it has been removed now that `values.join(separator)` is the only
spelling.

`Int`/`Float` are scalar `DiamondValue`s rather than heap objects, but native
dispatch provides a small method surface. Fixed duration helpers
`.second(s)()` through `.week(s)()` return numeric seconds, which compose with
`.ago()`/`.from_now()` to produce wall-clock `Time` values; see the
[Time guide](time.md).
General numeric helpers remain plain functions:
`abs(x)`/`min(a, b)`/`max(a, b)`/`mod(a, b)`, all accepting
`Int | Float` (mixed `Int`/`Float` arguments auto-promote, same as
arithmetic). `abs` inherits negation's overflow behavior, so `abs` of the
most negative 64-bit `Int` promotes to an arbitrary-precision `Int`
instead of raising or silently wrapping — there is no longer a most
negative `Int` that `abs` can't represent. `mod(a, b)` truncates `a / b` toward
zero before multiplying back (via `to_i`/`to_f` for the `Float` case,
since `/` between two `Float`s doesn't truncate the way `Int`
division does), so its result keeps the same C-style sign convention
for both types — `mod(-7, 3)` is `-1`, not `2` (not Euclidean/
Python-style mod), and `mod(-7.0, 3.0)` is likewise `-1.0`. `b == 0`
raises the same `ZeroDivisionError` integer division would; `b == 0.0`
raises a rescuable `RangeError` instead (from `to_i` rejecting the
resulting `Infinity`/`NaN` quotient — `Float` division by zero itself
never raises, only the truncation step does).

`sqrt(x)`, `sin(x)`, `cos(x)`, `tan(x)`, `exp(x)`, `log(x)`, `tanh(x)`, and
`pow(base, exponent)` are native functions (no bytecode primitive to build
on, same reasoning as `chr`/`to_f`/`to_i`) accepting `Int | Float` for every
argument and always returning `Float` — `pow(2, 10)` is `1024.0`, not
`1024`, even though both arguments are `Int`. No extra validation: results
follow IEEE-754 directly, so `sqrt(-1.0)` is `NaN` rather than an error, the
same philosophy `Float` arithmetic already uses throughout.

`Tensor` is a dense, row-major `Float` matrix — `Tensor.zeros(rows, cols)`,
`Tensor.from_array(nested_array)` (an `Array` of same-length `Array`s of
`Int`/`Float`), or `Tensor.random(rows, cols, seed)` (deterministic
pseudorandom values in `[-1, 1)` from a plain LCG, filled directly in C —
orders of magnitude faster than building the same values through
`Tensor.from_array` and a Diamond-level loop, which matters once "rows x
cols" reaches real model-weight sizes). Instance methods: `#rows()`,
`#cols()`, `#get(row, col)`, `#set(row, col, value)` (`IndexError` out of
bounds, matching `Array#[]`), `#matmul(other)` (threaded, k-blocked; shape
mismatch raises `TypeError`), `#transpose()` (a fresh copy), and
`#to_a()` (back to an ordinary nested `Array`). Deliberately a narrow
prototype, not a general tensor library: no broadcasting, no non-2D
shapes, no in-place ops, no autodiff — it exists to measure real `matmul`
throughput (`bench/`-style native code, not boxed `DiamondValue` arrays)
before committing to a fuller API surface.

`Time.monotonic()` returns a `Float` number of seconds from
`CLOCK_MONOTONIC` — a duration-only clock: the value itself means
nothing (not a calendar timestamp, not comparable across processes),
only the difference between two readings does, e.g. `elapsed =
Time.monotonic() - start` for timing a request in a rack middleware.
It is intentionally separate from Diamond's wall-clock/calendar `Time` values;
see the [Time guide](time.md) for those APIs.

`BCrypt.hash(password: String, cost: Int)` and `BCrypt.verify(password:
String, digest: String) -> Bool` are native bcrypt password hashing,
backed by this system's own `libxcrypt` (`crypt_gensalt_rn`/`crypt_r`,
real `$2b$` bcrypt) rather than a vendored implementation — the same
"link a system library" pattern every other native dependency here
already follows (`sqlite3`, `libpq`, `mariadb`, OpenSSL). `.hash` always
takes both arguments explicitly (no default `cost` at this layer — see
`packages/active_record/README.md`'s `#secure_password=` for where a
default of 12 actually lives); `cost` outside `4..31` raises
`ArgumentError` before any hashing happens. `.verify` re-derives a digest
from `password` using `digest` itself as the salt/settings source and
compares with a constant-time comparison — a malformed or foreign
`digest` (not a real bcrypt hash) is an ordinary `false`, not an
exception, since checking a password against a bad hash is a normal
outcome here, not a programmer error:

```ruby
digest = BCrypt.hash("hunter2", 12)   # => "$2b$12$..."
BCrypt.verify("hunter2", digest)      # => true
BCrypt.verify("wrong", digest)        # => false
```

`libxcrypt`'s bcrypt support is a Linux-specific fact about this system's
`crypt(3)`, not something Diamond papers over — see `docs/roadmap.md`'s
"Explicitly deferred" section on multi-platform portability. A password
containing an embedded NUL byte is truncated at that point before
hashing, the same inherent limitation every C-`crypt`-backed bcrypt
binding has (`password` is passed to `crypt_r` as a NUL-terminated C
string).

`SecureRandom.bytes(n: Int) -> String` returns `n` cryptographically
random bytes (OpenSSL `RAND_bytes`, already linked for TLS) as a raw
Diamond `String` — Diamond strings are already raw byte buffers, so no
separate binary type is needed. `SecureRandom.hex(n: Int) -> String`
returns the same `n` random bytes hex-encoded, as a `2*n`-character
`String`. Both raise `ArgumentError` for a negative `n` (or one large
enough to overflow the underlying `int`-sized call into OpenSSL).
Suited to session/remember-me/password-reset tokens and similar —
`SecureRandom.hex(32)` for a 256-bit token as a 64-character hex string.

`Digest.sha256(data: String) -> String` returns the lowercase hexadecimal
SHA-256 digest of the string's raw bytes. `HMAC.sha256(key: String, data:
String) -> String` returns the corresponding keyed HMAC, also as 64 lowercase
hexadecimal characters. Both preserve embedded NUL bytes and are backed by
OpenSSL's `libcrypto`:

```ruby
Digest.sha256("abc")
HMAC.sha256("secret", "payload")
```

`HMAC.verify(data: String, key: String, signature: String) -> Bool`
recomputes `HMAC.sha256(key, data)` and compares it against `signature`
with a constant-time comparison (`CRYPTO_memcmp`, the same primitive
`BCrypt.verify` already uses) rather than a plain `==` on the two hex
strings — verifying a signature is exactly the kind of comparison a
timing side-channel matters for, unlike an ordinary string equality
check. A `signature` of the wrong length, or not really hex at all, is
an ordinary `false`, not an exception, matching `BCrypt.verify`'s own
"a bad input here is a normal outcome" reasoning:

```ruby
signature = HMAC.sha256("secret", "payload")
HMAC.verify("payload", "secret", signature)   # => true
HMAC.verify("payload", "secret", "forged")    # => false
```

`Digest.sha1(data: String) -> String` and `HMAC.sha1(key: String, data:
String) -> String` are the same pair, using SHA-1 instead (40 lowercase
hexadecimal characters). SHA-1 is cryptographically weak for new signing
use — this exists because TOTP (RFC 6238) mandates HMAC-SHA1 by spec, not
as a second general-purpose recommendation alongside `sha256`. `HMAC.verify`
has no `sha1` counterpart; it only ever checks against `HMAC.sha256`.

`Cipher.encrypt(key: String, plaintext: String) -> String` and
`Cipher.decrypt(key: String, blob: String) -> String | Nil` are native
AES-256-GCM authenticated encryption, backed by OpenSSL's `EVP_CIPHER`
API (already linked for TLS) rather than a vendored implementation.
`key` must be exactly 32 raw bytes (`SecureRandom.bytes(32)`) —
`ArgumentError` otherwise, since AES-256 has no meaningful way to
silently accept a wrong-length key. `.encrypt` generates a fresh random
12-byte nonce every call and returns one binary-safe `String`: the
nonce, the 16-byte GCM authentication tag, then the ciphertext, all
concatenated — `.decrypt` needs nothing but the key and this one blob.
`.decrypt` returns `nil`, not an exception, for a wrong key, a tampered
or truncated blob, or anything else that fails GCM's own built-in tag
check — a forged or expired encrypted cookie is expected to happen in
normal operation, the same "not a programmer error" reasoning
`BCrypt.verify`/`HMAC.verify` already use, and there is no separate
signature step to add: the tag check inside `EVP_DecryptFinal_ex`
already provides both confidentiality and tamper-evidence in one pass,
with no comparison of Diamond's own to expose a timing side-channel.

```ruby
key = SecureRandom.bytes(32)
blob = Cipher.encrypt(key, "sensitive session data")
Cipher.decrypt(key, blob)             # => "sensitive session data"
Cipher.decrypt(SecureRandom.bytes(32), blob)  # => nil (wrong key)
```

`Gzip.compress(data: String) -> String` and `Gzip.decompress(data: String,
max_size: Int) -> String` are gzip-wrapped deflate, backed by the
already-linked zlib. `.decompress` also accepts a plain zlib-wrapped
stream (auto-detected) — some servers send `Content-Encoding: deflate`
as one of these rather than raw deflate, and this covers both real-world
spellings without the caller needing to know which. `max_size` bounds
the *decompressed* output, checked as it grows rather than after the
fact — decompressing a small, untrusted input into an unbounded output
("zip bomb") is a real risk for anything that automatically decompresses
a network response, so this raises a rescuable `IOError` instead of ever
fully materializing an over-cap buffer. Malformed input (corrupt or
truncated) also raises `IOError`, not `nil` — unlike `Cipher.decrypt`,
there's no expected-in-normal-operation forged-input case here to justify
that softer failure mode.

```ruby
compressed = Gzip.compress("some text")
Gzip.decompress(compressed, 10 * 1024 * 1024)  # => "some text"
```

`Base64.encode(data: String) -> String` and `Base64.decode(data: String)
-> String` are the standard (RFC 4648) alphabet with padding, via
OpenSSL's `EVP_EncodeBlock`/`EVP_DecodeBlock` (already linked). Needed
for HTTP Basic auth (`Authorization: Basic <base64>`). `.decode` validates
the character set itself before decoding (rather than trusting
`EVP_DecodeBlock`'s own lenience, which varies by OpenSSL version), so a
malformed input (wrong length, an invalid character, misplaced `=`
padding) always raises a rescuable `ArgumentError` with a clear message:

```ruby
Base64.encode("hello world")              # => "aGVsbG8gd29ybGQ="
Base64.decode("aGVsbG8gd29ybGQ=")          # => "hello world"
```

See `packages/cookies` for signed and encrypted cookie helpers built on
`HMAC.verify`/`Cipher` above.

`array_sort(values: Array[Int])` returns a new sorted array (input
untouched); `Int` is the only type with a native ordering comparison,
so this is Int-only, checked up front (`expected Array[Int], got
Array` on a non-Int element) rather than failing confusingly mid-sort.

`StringBuilder` (defined in the prelude, `lib/core.di`) is the named
escape hatch from `result = result + piece` in a loop — quadratic,
since it reallocates and copies the whole accumulated string on every
iteration:

```ruby
sb = StringBuilder.new()
sb.append("hello")
sb.append(", world")
sb.to_s()        # => "hello, world"
"#{sb}"           # same, via to_s
sb.length()       # => 12
```

`#append` returns `self`, so calls chain: `sb.append("a").append("b")`.
It's a plain Diamond class, not a native object — `#append` pushes
`"#{piece}"` onto an internal `Array` (already O(1) amortized) and
`#to_s` calls the native `.join("")` above once, so the total cost of
building a string this way is O(n), the same complexity `Array#join`
already has for a pre-collected array of pieces.
