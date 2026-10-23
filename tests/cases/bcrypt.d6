# BCrypt.hash/.verify -- backed by libxcrypt's crypt_gensalt_rn/crypt_r
# (src/vm.c), not a vendored implementation (this system's crypt(3)
# already speaks real $2b$ bcrypt -- see packages/active_record/ROADMAP.md
# and docs/syntax.md for the full story). Salted, so the digest itself is
# non-deterministic between runs -- every assertion here checks a
# property of the digest/verification, never the digest's own exact text.

digest = BCrypt.hash("hunter2", 4)
puts(digest.slice(0, 4))
puts(digest.length() > 20)

puts(BCrypt.verify("hunter2", digest))
puts(BCrypt.verify("wrong password", digest))

# Same password, two calls: different salt, different digest.
other_digest = BCrypt.hash("hunter2", 4)
puts(digest != other_digest)
puts(BCrypt.verify("hunter2", other_digest))

# A malformed/foreign digest is an ordinary non-match, not an exception.
puts(BCrypt.verify("hunter2", "not a real digest"))

begin
  BCrypt.hash("hunter2", 2)
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised")
end

begin
  BCrypt.hash("hunter2", 32)
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised")
end

puts("bcrypt smoke ok")
