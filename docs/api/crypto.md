# Crypto and encoding

[API reference](index.md) · Previous: [Databases](database.md) · Next: [Tensor](tensor.md)

Hashing, signing, encryption, random tokens, compression, Base64 and version
strings. Strings are byte buffers, so binary input and output are plain
Strings. The cryptography comes from OpenSSL, bcrypt from the system's
libxcrypt and gzip from zlib.

Checks that can fail on bad input in normal use (a wrong password, a forged
signature, a tampered ciphertext) return `false` or `nil` instead of raising.

## Digest

### `Digest.sha256(data) -> String`

The SHA-256 digest of `data`, as 64 lowercase hex characters.

```ruby
Digest.sha256("abc")   # => "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
```

### `Digest.sha1(data) -> String`

The SHA-1 digest, as 40 hex characters. SHA-1 is weak; use it only where a
protocol requires it.

## HMAC

### `HMAC.sha256(key, data) -> String`

The HMAC-SHA256 of `data` under `key`, as 64 hex characters.

### `HMAC.sha1(key, data) -> String`

The HMAC-SHA1, as 40 hex characters, for protocols such as TOTP.

### `HMAC.verify(data, key, signature) -> Bool`

Whether `signature` equals `HMAC.sha256(key, data)`, compared in constant
time. Note the argument order: `data` comes first.

```ruby
signature = HMAC.sha256("secret", "payload")
HMAC.verify("payload", "secret", signature)   # => true
HMAC.verify("payload", "secret", "forged")    # => false
```

## BCrypt

### `BCrypt.hash(password, cost) -> String`

A `$2b$` bcrypt hash of `password`. `cost` runs from `4` to `31`, each step
doubling the work; outside that range raises `ArgumentError`. A password is
cut at its first NUL byte.

### `BCrypt.verify(password, digest) -> Bool`

Whether `password` matches `digest`. A `digest` that is not a bcrypt hash
gives `false`.

```ruby
digest = BCrypt.hash("hunter2", 4)
BCrypt.verify("hunter2", digest)   # => true
BCrypt.verify("wrong", digest)     # => false
```

## Cipher

AES-256-GCM authenticated encryption.

### `Cipher.encrypt(key, plaintext) -> String`

Encrypts with a fresh random nonce and returns one binary String holding the
nonce, the authentication tag and the ciphertext. `key` must be 32 bytes,
or it raises `ArgumentError`.

### `Cipher.decrypt(key, blob) -> String | Nil`

The plaintext, or `nil` when the key is wrong or `blob` was altered.

```ruby
key = SecureRandom.bytes(32)
blob = Cipher.encrypt(key, "session data")
Cipher.decrypt(key, blob)                      # => "session data"
Cipher.decrypt(SecureRandom.bytes(32), blob)   # => nil
```

## SecureRandom

### `SecureRandom.bytes(n) -> String`

`n` cryptographically random bytes.

### `SecureRandom.hex(n) -> String`

`n` random bytes as `2 * n` hex characters.

```ruby
SecureRandom.hex(16).length()   # => 32
```

## Gzip

### `Gzip.compress(data) -> String`

`data` compressed in gzip format.

### `Gzip.decompress(data, max_size) -> String`

Decompresses gzip or zlib data. Raises `IOError` if the output would exceed
`max_size` bytes or the input is corrupt, so untrusted input cannot expand
without bound.

```ruby
Gzip.decompress(Gzip.compress("some text"), 1024)   # => "some text"
```

## Base64

### `Base64.encode(data) -> String`

Standard Base64 with padding.

### `Base64.decode(text) -> String`

The decoded bytes. Malformed input raises `ArgumentError`.

```ruby
Base64.encode("hello world")        # => "aGVsbG8gd29ybGQ="
Base64.decode("aGVsbG8gd29ybGQ=")   # => "hello world"
```

## SemVer

Semantic version strings, with the rules the `facet` package manager uses. A
leading `v` is allowed.

### `SemVer.valid?(text) -> Bool`

Whether `text` is a valid version.

### `SemVer.compare(a, b) -> Int`

`-1`, `0` or `1`. A prerelease sorts before its release, and build metadata
is ignored. A malformed version raises `ArgumentError`.

```ruby
SemVer.compare("1.2.0-rc.1", "1.2.0")   # => -1
```

### `SemVer.satisfies?(version, range) -> Bool`

Whether `version` is in `range`: `^1.2.0`, `~1.2.0` or comparisons such as
`>=1.0.0 <2.0.0`. Below 1.0 a caret range stays within one minor version.

```ruby
SemVer.satisfies?("0.4.1", "^0.4.0")   # => true
SemVer.satisfies?("0.5.0", "^0.4.0")   # => false
```
