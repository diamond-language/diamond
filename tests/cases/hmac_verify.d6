# HMAC.verify -- recomputes HMAC-SHA256 and compares against a given
# signature with OpenSSL's CRYPTO_memcmp (constant-time), the same
# primitive BCrypt.verify already uses for its own comparison.

sig = HMAC.sha256("secret", "payload")

puts(HMAC.verify("payload", "secret", sig))
puts(HMAC.verify("payload", "wrong secret", sig))
puts(HMAC.verify("tampered payload", "secret", sig))

# A malformed/foreign signature (wrong length, not real hex) is an
# ordinary non-match, not an exception -- same reasoning BCrypt.verify's
# own doc comment already gives for a malformed digest.
puts(HMAC.verify("payload", "secret", "not a real signature"))
puts(HMAC.verify("payload", "secret", ""))

# Raw bytes, including embedded NULs, work as data/key too.
binary = "a" + 0.chr() + "b"
binary_sig = HMAC.sha256("key", binary)
puts(HMAC.verify(binary, "key", binary_sig))

begin
  HMAC.verify(42, "secret", sig)
  puts("no raise")
rescue error: TypeError
  puts("data TypeError")
end

begin
  HMAC.verify("payload", 42, sig)
  puts("no raise")
rescue error: TypeError
  puts("key TypeError")
end

begin
  HMAC.verify("payload", "secret", 42)
  puts("no raise")
rescue error: TypeError
  puts("signature TypeError")
end

puts("hmac verify smoke ok")
