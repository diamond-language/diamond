header = [1, 513, 70000, -2, 258].pack("CSLcn")
header_back = header.unpack("CSLcn")
big = [4294967296 * 3 + 7].pack("Q")
big_back = big.unpack1("Q")
negative = [-1, -300].pack("lq").unpack("lq")
be = [3405691582].pack("N").unpack("C*")
text = ["ab", "cdef", "x"].pack("A4a3Z*").unpack("A4a3Z*")
hex = ["deadbeef"].pack("H*").unpack("C*")
hex_back = "\xde\xad\xbe\xef".unpack("H*")
utf = [233, 8364, 128512].pack("U*")
utf_back = utf.unpack("U*")
short = "\x01".unpack("S")
unknown = nil
begin
  [1].pack("y")
rescue e: ArgumentError
  unknown = e.message()
end
[header.length(), header_back, big.length(), big_back, negative, be, text, hex, hex_back, utf.length(), utf_back, short, unknown]
