# CRC-32 (the zlib/PNG polynomial), table-driven. Every PNG chunk ends
# with the CRC of its type and data, so writing a chunk means computing
# one. The 256-entry table is built once, on first use, and kept in a
# module class variable.
module Crc32
  def self.table() -> Array[Int]
    @@table = Crc32.build_table() if @@table == nil
    @@table
  end

  def self.build_table() -> Array[Int]
    (0...256).map() do |n|
      c = n
      8.times() do |_|
        c = if c & 1 == 1 then 0xEDB8_8320 ^ (c >> 1) else c >> 1 end
      end
      c
    end
  end

  def self.of(data: String) -> Int
    table = Crc32.table()
    crc = 0xFFFF_FFFF
    index = 0
    while index < data.length()
      crc = table[(crc ^ data.getbyte(index)) & 0xFF] ^ (crc >> 8)
      index += 1
    end
    crc ^ 0xFFFF_FFFF
  end
end
