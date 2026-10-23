puts(Digest.sha1(""))
puts(Digest.sha1("abc"))
puts(HMAC.sha1("key", "The quick brown fox jumps over the lazy dog"))

# Strings are raw bytes, including embedded NULs.
puts(Digest.sha1("a" + 0.chr() + "b"))

begin
  Digest.sha1(42)
  puts("no raise")
rescue error: TypeError
  puts("digest type error")
end

begin
  HMAC.sha1("key", 42)
  puts("no raise")
rescue error: TypeError
  puts("hmac type error")
end

puts("crypto sha1 smoke ok")
