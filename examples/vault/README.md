# examples/vault

An encrypted secrets file, built entirely from Diamond's crypto and
encoding builtins: `BCrypt`, `Cipher` (AES-256-GCM), `HMAC`, `Digest`,
`SecureRandom`, `Base64`, `Gzip`, and `JSON`.

```text
$ export VAULT_PASSWORD='correct horse'
$ diamond vault.di secrets.vault init
created secrets.vault
$ echo 'hunter2' | diamond vault.di secrets.vault add wifi
stored wifi
$ diamond vault.di secrets.vault get wifi
hunter2
$ VAULT_PASSWORD=nope diamond vault.di secrets.vault get wifi
vault: wrong password
$ diamond vault.di secrets.vault verify
ok
```

Commands: `init`, `add NAME` (secret on stdin), `get NAME`, `list`,
`rm NAME`, and `verify`. The password comes from `VAULT_PASSWORD`, since
Diamond has no way to read a terminal line without echoing it. Exit status
is 0 on success, 1 for a vault error (wrong password, unknown entry,
tampering), 64 for a usage error, and 66 when the file can't be read.

## How it works

The vault file is JSON (`JSON.stringify` / `JSON.parse`):

- `check`: a bcrypt hash of the password (`BCrypt.hash`, verified with the
  constant-time `BCrypt.verify`).
- `salt` and `rounds`: inputs to the key derivation. The 32-byte AES key is
  SHA-256 applied `rounds` times to the salted password (`derive_key` in
  `lib/crypto.di`). This stands in for a real KDF such as PBKDF2, scrypt,
  or Argon2, which Diamond doesn't provide.
- `entries`: each secret is gzipped (`Gzip.compress`), encrypted with
  AES-256-GCM under a fresh random nonce (`Cipher.encrypt`), and
  Base64-encoded. GCM authenticates as well as encrypts, so
  `Cipher.decrypt` returns `nil` for a wrong key or any edited byte.
- `mac`: an HMAC-SHA256 over all entries (`HMAC.sha256`, checked with the
  constant-time `HMAC.verify`). This catches what per-entry encryption
  can't: an entry copied under another name, or one deleted.

## What it shows

- **The crypto builtins together**, including the nil-on-failure contract
  of `Cipher.decrypt` and the constant-time verify functions.
- **Binary data in Strings.** A Diamond String is a byte buffer, so raw key
  bytes (`hex_to_bytes` builds them with `chr`) and ciphertext need no
  separate binary type.
- **Typed `nil` handling.** `unseal` returns `String | Nil`;
  `Vault#get` narrows it with a one-line guard
  (`raise ... if secret == nil`) before returning a plain `String`.
- **A command dispatcher** that matches `[command, *rest]` against Array
  patterns such as `["add", name]`.
- **Error handling** through one rescue in `main` for `VaultError` or
  `JSONError` (exit 1) and `IOError` (exit 66).

Two gaps this example works around: `File.publish` won't replace an
existing file and there's no `File.rename`, so `save` rewrites the file in
place rather than atomically; and there's no no-echo password prompt.

## Test

```sh
bash smoke_test.sh
```

This stores and reads secrets under the interpreter and as a
`diamond build` binary, checks nothing sensitive is stored in the clear,
and tampers with a vault two ways: a flipped ciphertext byte (GCM rejects
it) and an entry copied under another name (only the HMAC catches it).
