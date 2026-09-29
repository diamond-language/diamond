# The container format's own primitives: big-endian fixed-width integers
# (the same [(n >> 24) & 0xFF, ...] idiom examples/pngmeta uses to write a
# PNG chunk length), and packing/unpacking a String of '0'/'1' characters
# into real bytes, most-significant-bit first.

def pack_u32(value: Int) -> String
  bytes = [(value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF]
  bytes.map() do |byte| byte.chr() end.join("")
end

def unpack_u32(bytes: String, at: Int) -> Int
  bytes.getbyte(at) * 16777216 + bytes.getbyte(at + 1) * 65536 +
    bytes.getbyte(at + 2) * 256 + bytes.getbyte(at + 3)
end

def pack_u16(value: Int) -> String
  [(value >> 8) & 0xFF, value & 0xFF].map() do |byte| byte.chr() end.join("")
end

def unpack_u16(bytes: String, at: Int) -> Int
  bytes.getbyte(at) * 256 + bytes.getbyte(at + 1)
end

# Pads the final byte with 0 bits -- harmless, since a decoder always
# knows how many *symbols* to expect and stops there, never how many
# bits, so it never looks at padding past the last real code.
def pack_bits(bits: String) -> String
  out = []
  index = 0
  while index < bits.length()
    byte = 0
    offset = 0
    while offset < 8
      bit = index + offset < bits.length() && bits[index + offset] == "1" ? 1 : 0
      byte = byte * 2 + bit
      offset += 1
    end
    out.push(byte.chr())
    index += 8
  end
  out.join("")
end

def unpack_bits(bytes: String) -> String
  out = ""
  index = 0
  while index < bytes.length()
    byte = bytes.getbyte(index)
    bit = 7
    while bit >= 0
      out = out + (((byte >> bit) & 1) == 1 ? "1" : "0")
      bit -= 1
    end
    index += 1
  end
  out
end
