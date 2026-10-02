# CRC-32 (the zlib/PNG polynomial), table-driven. Every PNG chunk ends
# with the CRC of its type and data, so writing a chunk means computing
# one. The 256-entry table is built once, on first use, and kept in a
# module class variable.
module Crc32
  # The lookup table, built on first use and cached in `@@table` (a class
  # variable on the module, which starts out nil).
  def self.table() -> Array[Int]
    @@table = Crc32.build_table() if @@table == nil
    @@table
  end

  # Entry n is the CRC of the single byte n. The CRC is polynomial division
  # one bit at a time: for each of the 8 bits, shift right, and whenever the
  # bit shifted out was 1, XOR in the polynomial 0xEDB88320 (the bit-reversed
  # form of the standard CRC-32 polynomial, because this variant processes
  # the least significant bit first). The table precomputes those 8 steps
  # for every possible byte so `of` needs only one lookup per input byte.
  def self.build_table() -> Array[Int]
    (0...256).map() do |n|
      c = n
      8.times() do |_|
        c = if c & 1 == 1 then 0xEDB8_8320 ^ (c >> 1) else c >> 1 end
      end
      c
    end
  end

  # The CRC-32 of a string. It starts from all ones and ends by inverting
  # all bits (the "pre- and post-conditioning" that the standard calls for,
  # which makes leading zero bytes affect the result).
  def self.of(data: String) -> Int
    table = Crc32.table()
    crc = 0xFFFF_FFFF
    index = 0

    # Fold in one byte at a time: the table entry chosen by the low byte of
    # (crc XOR input byte), XORed with the crc shifted down a byte.
    while index < data.length()
      crc = table[(crc ^ data.getbyte(index)) & 0xFF] ^ (crc >> 8)
      index += 1
    end
    crc ^ 0xFFFF_FFFF
  end
end
