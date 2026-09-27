# Reading and writing PNG files at the chunk level. A PNG is an 8-byte
# signature followed by chunks, each laid out as
#
#   length (4 bytes, big-endian) | type (4 ASCII letters) | data | CRC-32
#
# where the CRC covers the type and data. IHDR comes first and IEND last.
# See https://www.w3.org/TR/png/#5Chunk-layout.
require "./crc32"

class PngError < StandardError
end

struct Chunk(kind: String, data: String, crc: Int)
  # Does the stored CRC match the chunk's contents?
  def intact?() -> Bool = Crc32.of(kind() + data()) == crc()

  # A lowercase first letter marks an ancillary chunk: one a decoder may
  # skip. IHDR, PLTE, IDAT, and IEND are critical.
  def ancillary?() -> Bool = Regexp.new("^[a-z]").match?(kind())
end

def png_signature() -> String = "\x89PNG\r\n\x1a\n"

def read_u32(bytes: String, at: Int) -> Int
  (bytes.getbyte(at) << 24) | (bytes.getbyte(at + 1) << 16) |
    (bytes.getbyte(at + 2) << 8) | bytes.getbyte(at + 3)
end

def read_u16(bytes: String, at: Int) -> Int = (bytes.getbyte(at) << 8) | bytes.getbyte(at + 1)

def u32_bytes(n: Int) -> String
  [(n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF].map() do |b| b.chr() end.join("")
end

def make_chunk(kind: String, data: String) -> Chunk = Chunk.new(kind, data, Crc32.of(kind + data))

def encode_chunk(chunk: Chunk) -> String
  u32_bytes(chunk.data().length()) + chunk.kind() + chunk.data() + u32_bytes(chunk.crc())
end

def parse_png(bytes: String) -> Array[Chunk]
  unless bytes.slice(0, 8) == png_signature()
    raise PngError.new("not a PNG file (bad signature)")
  end
  chunks = []
  at = 8
  loop do
    raise PngError.new("truncated: no IEND chunk") if at == bytes.length()
    raise PngError.new("truncated chunk header at byte #{at}") if at + 8 > bytes.length()
    length = read_u32(bytes, at)
    kind = bytes.slice(at + 4, 4)
    unless Regexp.new("^[A-Za-z]{4}$").match?(kind)
      raise PngError.new("bad chunk type at byte #{at + 4}")
    end
    if at + 12 + length > bytes.length()
      raise PngError.new("truncated #{kind} chunk at byte #{at}")
    end
    chunks.push(Chunk.new(kind, bytes.slice(at + 8, length), read_u32(bytes, at + 8 + length)))
    at += 12 + length
    break if kind == "IEND"
  end
  raise PngError.new("first chunk is #{chunks[0].kind()}, not IHDR") unless chunks[0].kind() == "IHDR"
  chunks
end

def encode_png(chunks: Array[Chunk]) -> String
  chunks.reduce(png_signature()) do |bytes, chunk| bytes + encode_chunk(chunk) end
end

# --- What the chunks mean -------------------------------------------------

struct Header(width: Int, height: Int, bit_depth: Int, color_type: Int, interlaced: Bool)
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

def time_text(chunk: Chunk) -> String
  data = chunk.data()
  "%04d-%02d-%02d %02d:%02d:%02d UTC".format([read_u16(data, 0), data.getbyte(2),
    data.getbyte(3), data.getbyte(4), data.getbyte(5), data.getbyte(6)])
end

def metadata_kinds() -> Array[String] = ["tEXt", "zTXt", "iTXt", "tIME", "eXIf"]
