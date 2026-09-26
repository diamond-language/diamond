# Key handling for the vault. Everything cryptographic is a Diamond
# builtin backed by OpenSSL/libxcrypt: BCrypt, Cipher (AES-256-GCM), HMAC,
# Digest, SecureRandom, Base64, and Gzip.

class VaultError < StandardError
end

# "0aff" -> the two bytes 0x0a 0xff.
def hex_to_bytes(hex: String) -> String
  digits = "0123456789abcdef"
  bytes = StringBuilder.new()
  index = 0
  while index + 1 < hex.length()
    high = digits.index_of(hex[index])
    low = digits.index_of(hex[index + 1])
    raise VaultError.new("not hex: #{hex}") if high == nil || low == nil
    bytes.append(chr(high * 16 + low))
    index += 2
  end
  bytes.to_s()
end

# A 32-byte AES key from a password and salt: SHA-256 applied `rounds`
# times, so guessing passwords costs `rounds` hashes each. This stands in
# for a real KDF (PBKDF2, scrypt, Argon2), which Diamond doesn't provide.
def derive_key(password: String, salt: String, rounds: Int) -> String
  digest = Digest.sha256("#{salt}:#{password}")
  (rounds - 1).times() do |_|
    digest = Digest.sha256("#{salt}:#{digest}")
  end
  hex_to_bytes(digest)
end

# Compress, then encrypt (GCM authenticates too), then Base64 for JSON.
def seal(key: String, secret: String) -> String
  Base64.encode(Cipher.encrypt(key, Gzip.compress(secret)))
end

# The inverse of seal, or nil when the key is wrong or the data was
# tampered with -- GCM's tag check fails either way.
def unseal(key: String, sealed: String) -> String | Nil
  blob = Cipher.decrypt(key, Base64.decode(sealed))
  return nil if blob == nil
  Gzip.decompress(blob, 1_048_576)
end
