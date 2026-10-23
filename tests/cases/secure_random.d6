# SecureRandom.bytes/.hex -- OpenSSL RAND_bytes (src/vm.c), already linked
# for TLS. Output is non-deterministic by design, so every assertion here
# checks a property (length, distinctness, hex format) rather than exact
# bytes.

bytes16 = SecureRandom.bytes(16)
puts(bytes16.length())

other_bytes16 = SecureRandom.bytes(16)
puts(bytes16 != other_bytes16)

puts(SecureRandom.bytes(0).length())

hex8 = SecureRandom.hex(8)
puts(hex8.length())
puts(Regexp.new("\\A[0-9a-f]{16}\\z").match?(hex8))

other_hex8 = SecureRandom.hex(8)
puts(hex8 != other_hex8)

puts(SecureRandom.hex(0).length())

begin
  SecureRandom.bytes(-1)
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised")
end

begin
  SecureRandom.hex(-1)
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised")
end

puts("secure_random smoke ok")
