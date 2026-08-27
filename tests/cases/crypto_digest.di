puts(Digest.sha256(""))
puts(Digest.sha256("abc"))
puts(HMAC.sha256("key", "The quick brown fox jumps over the lazy dog"))

# Strings are raw bytes, including embedded NULs.
puts(Digest.sha256("a" + 0.chr() + "b"))

begin
  Digest.sha256(42)
  puts("no raise")
rescue error: TypeError
  puts("digest type error")
end

begin
  HMAC.sha256("key", 42)
  puts("no raise")
rescue error: TypeError
  puts("hmac type error")
end

puts("crypto digest smoke ok")
