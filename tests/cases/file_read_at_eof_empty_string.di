# File#read past the end of the stream builds an empty StringBuilder whose
# `chars` buffer was never allocated (stays null); allocate_string then
# called memcpy(dest, null, 0) -- undefined behavior by the C standard even
# though it copies zero bytes, and a real crash under UBSan's null-check.
File.write("/tmp/diamond_test_eof_empty.txt", "")
file = File.open("/tmp/diamond_test_eof_empty.txt", "r")
puts(file.read().length())
file.close()
File.write("/tmp/diamond_test_eof_empty.txt", "abc")
file = File.open("/tmp/diamond_test_eof_empty.txt", "r")
puts(file.read().length())
puts(file.read().length())
file.close()

# gets() on an empty line hits the same allocate_string(nullptr, 0) path
# once it has seen at least one character on the line before the newline
# -- an entirely blank line is nil, not an empty string, so this needs a
# line that already has one byte read then immediately ends.
File.write("/tmp/diamond_test_eof_empty2.txt", "\n\na\n")
file = File.open("/tmp/diamond_test_eof_empty2.txt", "r")
puts(file.gets().length())
puts(file.gets().length())
puts(file.gets().length())
file.close()
