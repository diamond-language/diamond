data = ""
i = 0
while i < 200
  data = data + "hello world "
  i = i + 1
end

compressed = Gzip.compress(data)
puts(compressed.length() < data.length())
puts(Gzip.decompress(compressed, 1000000) == data)

# Empty string round-trips too.
puts(Gzip.decompress(Gzip.compress(""), 1000000) == "")

begin
  Gzip.decompress(compressed, 10)
  puts("no raise")
rescue error: IOError
  puts("size cap raised")
end

begin
  Gzip.decompress("not a gzip stream", 1000000)
  puts("no raise")
rescue error: IOError
  puts("corrupt data raised")
end

begin
  Gzip.compress(42)
  puts("no raise")
rescue error: TypeError
  puts("compress type error")
end

begin
  Gzip.decompress(42, 1000000)
  puts("no raise")
rescue error: TypeError
  puts("decompress type error")
end

puts("gzip smoke ok")
