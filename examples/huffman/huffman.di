# huffman: lossless compression by Huffman coding.
#
# --encode IN OUT   compresses IN into OUT
# --decode IN OUT   reverses it
#
# Container format (all integers big-endian):
#   magic "HUF1" (4 bytes)
#   original length in bytes (u32)
#   symbol count (u16)
#   for each symbol: the byte value (1 byte), its code's bit length
#     (1 byte), then that many bits, padded to a byte boundary
#   the encoded data itself, as packed bits
#
# The code TABLE is what's stored, not the frequency table -- a decoder
# never rebuilds the Huffman tree at all, so there's no risk of it
# reconstructing a *different* but equally-valid tree over the same
# frequencies (a real possibility whenever two symbols tie on frequency)
# and decoding garbage as a result.
require "./lib/tree"
require "./lib/bits"

class HuffmanError < StandardError
end

# A top-level constant isn't visible inside a function, so this is a
# function rather than a top-level `MAGIC = "HUF1"` assignment.
def magic() -> String = "HUF1"

def usage() -> Int
  warn("usage: huffman.di --encode IN OUT")
  warn("       huffman.di --decode IN OUT")
  64
end

def byte_frequencies(data: String) -> Hash
  freqs = {}
  index = 0
  while index < data.length()
    byte = data.getbyte(index)
    freqs[byte] = freqs.fetch(byte, 0) + 1
    index += 1
  end
  freqs
end

def encode(input_path: String, output_path: String) -> Int
  data = File.read(input_path)
  if data.empty?()
    raise HuffmanError.new("cannot encode an empty file")
  end
  codes = {}
  huffman_collect_codes(huffman_build_tree(byte_frequencies(data)), "", codes)
  bits = ""
  index = 0
  while index < data.length()
    bits = bits + codes[data.getbyte(index)]
    index += 1
  end
  table = pack_u16(codes.length())
  codes.keys().each() do |byte|
    code = codes[byte]
    table = table + byte.chr() + code.length().chr() + pack_bits(code)
  end
  container = magic() + pack_u32(data.length()) + table + pack_bits(bits)
  File.write(output_path, container)
  ratio = container.length() * 100 / data.length()
  puts("#{input_path}: #{data.length()} -> #{container.length()} bytes (#{ratio}%)")
  0
end

def read_code_table(container: String, at: Int, symbol_count: Int) -> Array
  codes_by_string = {}
  position = at
  index = 0
  while index < symbol_count
    byte = container.getbyte(position)
    length = container.getbyte(position + 1)
    position += 2
    packed_length = (length + 7) / 8
    code = unpack_bits(container.slice(position, packed_length)).slice(0, length)
    codes_by_string[code] = byte
    position += packed_length
    index += 1
  end
  [codes_by_string, position]
end

def decode(input_path: String, output_path: String) -> Int
  container = File.read(input_path)
  unless container.length() >= 10 && container.slice(0, 4) == magic()
    raise HuffmanError.new("not a huffman container (bad magic)")
  end
  original_length = unpack_u32(container, 4)
  symbol_count = unpack_u16(container, 8)
  table = read_code_table(container, 10, symbol_count)
  codes_by_string = table[0]
  bits = unpack_bits(container.slice(table[1], container.length() - table[1]))
  output = []
  current = ""
  bit_index = 0
  while output.length() < original_length
    if bit_index >= bits.length()
      raise HuffmanError.new("truncated data: expected #{original_length} bytes, got #{output.length()}")
    end
    current = current + bits[bit_index]
    bit_index += 1
    if codes_by_string.include_key?(current)
      output.push(codes_by_string[current].chr())
      current = ""
    end
  end
  File.write(output_path, output.join(""))
  puts("#{input_path}: decoded #{original_length} bytes")
  0
end

def main(argv) -> Int
  return usage() unless argv.length() == 3
  begin
    case argv[0]
    when "--encode" then encode(argv[1], argv[2])
    when "--decode" then decode(argv[1], argv[2])
    else usage()
    end
  rescue error: IOError
    warn(error.message())
    66
  rescue error: HuffmanError
    warn(error.message())
    65
  end
end

exit(main(ARGV))
