# Builtin API reference

One page per built-in type, listing every method with its signature, what it
returns and an example. Every example on these pages runs in CI, and a check
fails the build when a method the runtime implements has no entry here.

The [language reference](../syntax.md) explains the rules these methods follow
(typing, blocks, errors); this part lists them.

## Values

- [Kernel functions](kernel.md): `puts`, `gets`, `exit`, ...
- [Object](object.md): methods every value answers; Nil, Bool and Symbol
- [String](string.md)
- [Array](array.md)
- [Hash](hash.md)
- [Int](int.md)
- [Float](float.md)
- [Range](range.md)

## Modules and classes in the prelude

- [Enumerable](enumerable.md)
- [Comparable](comparable.md)
- [LazyEnumerator](lazy_enumerator.md)
- [StringBuilder](string_builder.md)
- [Math](math.md)
- [JSON](json.md)
- [URI](uri.md)

## Files and the system

- [File](file.md)
- [Dir](dir.md)
- [Time](time.md)
- [Regexp](regexp.md)
- [MatchData](match_data.md)
- [Process, Signal and IO](process.md)

## Concurrency

- [Fiber](fiber.md)
- [Thread](thread.md)
- [Channel](channel.md)
- [Supervisor](supervisor.md)

## Networking

- [TCP, TLS, UDP and DNS](network.md)
- [SQLite3, PostgreSQL and MySQL](database.md)

## Utilities

- [Crypto and encoding](crypto.md): Digest, HMAC, BCrypt, Cipher, SecureRandom, Gzip, Base64, SemVer
- [Tensor](tensor.md)
