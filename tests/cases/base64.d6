puts(Base64.encode("hello world"))
puts(Base64.decode("aGVsbG8gd29ybGQ="))
puts(Base64.decode(Base64.encode("user:pass")) == "user:pass")
puts(Base64.encode(""))
puts(Base64.decode(""))
puts(Base64.encode("a"))
puts(Base64.encode("ab"))
puts(Base64.encode("abc"))

# Strings are raw bytes, including embedded NULs.
puts(Base64.decode(Base64.encode("a" + 0.chr() + "b")) == "a" + 0.chr() + "b")

begin
  Base64.decode("abc")
  puts("no raise")
rescue error: ArgumentError
  puts("length error")
end

begin
  Base64.decode("ab!=")
  puts("no raise")
rescue error: ArgumentError
  puts("char error")
end

begin
  Base64.encode(42)
  puts("no raise")
rescue error: TypeError
  puts("encode type error")
end

begin
  Base64.decode(42)
  puts("no raise")
rescue error: TypeError
  puts("decode type error")
end

puts("base64 smoke ok")
