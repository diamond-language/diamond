# The container format's own primitives: big-endian fixed-width integers
# (the same [(n >> 24) & 0xFF, ...] idiom examples/pngmeta uses to write a
# PNG chunk length), and packing/unpacking a String of '0'/'1' characters
# into real bytes, most-significant-bit first.

# Writes a 32-bit integer as 4 bytes, most significant first: shift each byte
# into the low 8 bits and mask the rest off with 0xFF.
def pack_u32(value: Int) -> String
  bytes = [(value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF]
  bytes.map() do |byte| byte.chr() end.join("")
end

# Reads 4 bytes back into an integer, as base-256 digits (16777216 = 256^3,
# 65536 = 256^2). Multiplication rather than shifts keeps the value positive
# and exact.
def unpack_u32(bytes: String, at: Int) -> Int
  bytes.getbyte(at) * 16777216 + bytes.getbyte(at + 1) * 65536 +
    bytes.getbyte(at + 2) * 256 + bytes.getbyte(at + 3)
end

# The same two operations for 16 bits.
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

  # One output byte per 8 input characters.
  while index < bits.length()
    byte = 0
    offset = 0

    # Build the byte most-significant-bit first: shift what we have left one
    # place and add the next bit. Positions past the end of the input count
    # as 0, which is the padding.
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

# The inverse: every byte becomes 8 characters of "0"/"1", high bit first.
# (It cannot tell padding from data; callers trim to the length they know.)
def unpack_bits(bytes: String) -> String
  out = ""
  index = 0

  while index < bytes.length()
    byte = bytes.getbyte(index)
    bit = 7

    # Test bit 7 down to bit 0: shift it to the bottom and mask with 1.
    while bit >= 0
      out = out + (((byte >> bit) & 1) == 1 ? "1" : "0")
      bit -= 1
    end
    index += 1
  end
  out
end
