# Cipher.encrypt/.decrypt -- AES-256-GCM via OpenSSL's EVP_CIPHER API
# (src/vm.c), not a vendored implementation. The nonce is random per
# call, so the blob itself is non-deterministic between runs -- every
# assertion here checks a property of the blob/round-trip, never its
# exact bytes.

key = SecureRandom.bytes(32)
plaintext = "hello, secure cookies"
blob = Cipher.encrypt(key, plaintext)

# nonce(12) + tag(16) + ciphertext(same length as plaintext for GCM).
puts(blob.length() == 12 + 16 + plaintext.length())
puts(Cipher.decrypt(key, blob) == plaintext)

# Same plaintext, two calls: different random nonce, different blob.
other_blob = Cipher.encrypt(key, plaintext)
puts(blob != other_blob)
puts(Cipher.decrypt(key, other_blob) == plaintext)

# Empty plaintext round-trips too.
puts(Cipher.decrypt(key, Cipher.encrypt(key, "")) == "")

# Raw bytes, including embedded NULs, round-trip.
binary = "a" + 0.chr() + "b"
puts(Cipher.decrypt(key, Cipher.encrypt(key, binary)) == binary)

# A tampered blob fails GCM's own tag check -- nil, not an exception.
# The replacement byte must be guaranteed to differ from the original
# (a fixed "X" only tampers ~255/256 of the time -- the random tag byte
# at this position occasionally already equals "X", making it a no-op
# "tamper" that legitimately round-trips, an intermittent false test
# failure with no bug behind it, confirmed directly: 500 runs of a
# fixed-byte version reproduced it once).
original_byte = blob.slice(20, 21).ord()
tamper_byte = ((original_byte + 1) % 256).chr()
tampered = blob.slice(0, 20) + tamper_byte + blob.slice(21, blob.length())
puts(Cipher.decrypt(key, tampered))

# The wrong key is an ordinary decrypt failure too.
puts(Cipher.decrypt(SecureRandom.bytes(32), blob))

# A too-short blob (can't even hold nonce+tag) is nil, not a crash.
puts(Cipher.decrypt(key, "short"))

begin
  Cipher.encrypt("too short", plaintext)
  puts("no raise")
rescue error: ArgumentError
  puts("encrypt key-length ArgumentError")
end

begin
  Cipher.decrypt("too short", blob)
  puts("no raise")
rescue error: ArgumentError
  puts("decrypt key-length ArgumentError")
end

begin
  Cipher.encrypt(key, 42)
  puts("no raise")
rescue error: TypeError
  puts("encrypt plaintext TypeError")
end

puts("cipher aes-gcm smoke ok")
