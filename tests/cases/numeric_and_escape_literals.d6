# Hex, binary, and octal Int literals (with _ separators), and the
# \0 \e \xHH \uHHHH \u{H...} string escapes.
numbers = [0xff, 0XFF, 0b1010_1010, 0o17, 0x7fff_ffff, 017, 0]
bytes = ["\x89PNG\r\n\x1a\n".bytes(), "\0\e\x7".bytes()]
unicode = ["é".bytes(), "\u{1F600}".bytes(), "\u{41}\u{42}", "café" == "café"]
[numbers, bytes, unicode]
