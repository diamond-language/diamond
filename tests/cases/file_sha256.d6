path = "/tmp/diamond_file_sha256_test.bin"
File.write(path, "abc")
abc = File.sha256(path)

# Larger than the 64 KiB read chunk and not a multiple of the 64-byte block, so chunk and
# block boundaries both fall mid-stream.
big = "0123456789abcdef" * 20001 + "xyz"
File.write(path, big)
matches_one_shot = File.sha256(path) == Digest.sha256(big)

File.write(path, "")
empty = File.sha256(path)
File.delete(path)

missing = begin
  File.sha256("/nonexistent/diamond_file_sha256")
rescue e: IOError
  "IOError"
end

[abc, matches_one_shot, empty, missing]
