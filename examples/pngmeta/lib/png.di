# Reading and writing PNG files at the chunk level. A PNG is an 8-byte
# signature followed by chunks, each laid out as
#
#   length (4 bytes, big-endian) | type (4 ASCII letters) | data | CRC-32
#
# where the CRC covers the type and data. IHDR comes first and IEND last.
# See https://www.w3.org/TR/png/#5Chunk-layout.
require "./crc32"

# The file is not a well-formed PNG (or is damaged).
class PngError < StandardError
end

# One chunk as stored: type, data, and the CRC that was in the file (not a
# recomputed one, so `intact?` can detect corruption).
struct Chunk(kind: String, data: String, crc: Int)
  # Does the stored CRC match the chunk's contents?
  def intact?() -> Bool = Crc32.of(kind() + data()) == crc()

  # A lowercase first letter marks an ancillary chunk: one a decoder may
  # skip. IHDR, PLTE, IDAT, and IEND are critical.
  def ancillary?() -> Bool = Regexp.new("^[a-z]").match?(kind())
end

# The 8 bytes every PNG starts with. \x89 (a non-ASCII byte) and the \r\n
# \x1a \n sequence are chosen so that transfer mistakes (stripping the high
# bit, newline conversion) make the signature visibly wrong.
def png_signature() -> String = "\x89PNG\r\n\x1a\n"

# Big-endian integers: the first byte is the most significant. Shift each
# byte into place and OR them together.
def read_u32(bytes: String, at: Int) -> Int
  (bytes.getbyte(at) << 24) | (bytes.getbyte(at + 1) << 16) |
    (bytes.getbyte(at + 2) << 8) | bytes.getbyte(at + 3)
end

def read_u16(bytes: String, at: Int) -> Int = (bytes.getbyte(at) << 8) | bytes.getbyte(at + 1)

# The inverse: an integer as 4 big-endian bytes.
def u32_bytes(n: Int) -> String
  [(n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF].map() do |b| b.chr() end.join("")
end

# A new chunk with its CRC computed (the CRC covers the type and data, not
# the length).
def make_chunk(kind: String, data: String) -> Chunk = Chunk.new(kind, data, Crc32.of(kind + data))

# A chunk as bytes: length, type, data, CRC.
def encode_chunk(chunk: Chunk) -> String
  u32_bytes(chunk.data().length()) + chunk.kind() + chunk.data() + u32_bytes(chunk.crc())
end

# Splits a PNG into chunks, checking the structure (but NOT the CRCs: see
# `intact?` and pngmeta.di's load_intact, since `info` wants to report a bad
# CRC rather than refuse the file).
def parse_png(bytes: String) -> Array[Chunk]
  unless bytes.slice(0, 8) == png_signature()
    raise PngError.new("not a PNG file (bad signature)")
  end
  chunks = []
  at = 8

  # Walk chunk by chunk until IEND, which is the last. Running out of bytes
  # first means the file was cut off, and each check below guards a
  # different way that can happen.
  loop do
    raise PngError.new("truncated: no IEND chunk") if at == bytes.length()
    raise PngError.new("truncated chunk header at byte #{at}") if at + 8 > bytes.length()

    # Header: 4-byte length, 4-letter type. A chunk occupies 12 bytes plus
    # its data (length, type, CRC are 4 bytes each).
    length = read_u32(bytes, at)
    kind = bytes.slice(at + 4, 4)
    unless Regexp.new("^[A-Za-z]{4}$").match?(kind)
      raise PngError.new("bad chunk type at byte #{at + 4}")
    end
    if at + 12 + length > bytes.length()
      raise PngError.new("truncated #{kind} chunk at byte #{at}")
    end

    # Data starts after the 8 header bytes; the stored CRC follows it.
    chunks.push(Chunk.new(kind, bytes.slice(at + 8, length), read_u32(bytes, at + 8 + length)))
    at += 12 + length
    break if kind == "IEND"
  end

  # The PNG spec requires the header chunk first.
  raise PngError.new("first chunk is #{chunks[0].kind()}, not IHDR") unless chunks[0].kind() == "IHDR"
  chunks
end

# The whole file: the signature, then each chunk in order.
def encode_png(chunks: Array[Chunk]) -> String
  chunks.reduce(png_signature()) do |bytes, chunk| bytes + encode_chunk(chunk) end
end

# --- What the chunks mean -------------------------------------------------

# The decoded IHDR chunk.
struct Header(width: Int, height: Int, bit_depth: Int, color_type: Int, interlaced: Bool)
  # The PNG spec's numeric color types, in words.
  def color_name() -> String
    case color_type()
    when 0 then "grayscale"
    when 2 then "RGB"
    when 3 then "palette"
    when 4 then "grayscale+alpha"
    when 6 then "RGBA"
    else "unknown (#{color_type()})"
    end
  end
end

# IHDR is exactly 13 bytes: width (4), height (4), bit depth (1), color type
# (1), compression (1), filter (1), interlace (1). Compression and filter are
# always 0 in PNG, so they are skipped; interlace is 1 when interlaced.
def header_of(chunks: Array[Chunk]) -> Header
  data = chunks[0].data()
  raise PngError.new("IHDR is #{data.length()} bytes, not 13") unless data.length() == 13
  Header.new(read_u32(data, 0), read_u32(data, 4), data.getbyte(8), data.getbyte(9),
    data.getbyte(12) == 1)
end

# tEXt is "keyword NUL text", both Latin-1.
def text_entry(chunk: Chunk) -> Array[String] | Nil
  return nil unless chunk.kind() == "tEXt"
  parts = chunk.data().split("\0")
  return nil unless parts.length() == 2
  parts
end

# tIME is a 2-byte year, then month, day, hour, minute, second as one byte
# each, always in UTC.
def time_text(chunk: Chunk) -> String
  data = chunk.data()
  "%04d-%02d-%02d %02d:%02d:%02d UTC".format([read_u16(data, 0), data.getbyte(2),
    data.getbyte(3), data.getbyte(4), data.getbyte(5), data.getbyte(6)])
end

# The chunks `strip` removes: text (plain, compressed, international), the
# modification time, and EXIF data. None of them affect the picture.
def metadata_kinds() -> Array[String] = ["tEXt", "zTXt", "iTXt", "tIME", "eXIf"]
