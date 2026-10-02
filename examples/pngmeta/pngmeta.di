# pngmeta: inspect and edit PNG metadata without touching the pixels.
#
#   diamond pngmeta.di info FILE...          dimensions, chunks, text, CRCs
#   diamond pngmeta.di strip IN OUT          drop text, time, and EXIF chunks
#   diamond pngmeta.di set IN OUT KEY VALUE  add or replace a tEXt entry
#
# Exit status: 0 on success, 1 when a file isn't a valid PNG (bad
# signature, truncated, or a chunk whose CRC doesn't match), 64 for a
# usage error, 66 when a file can't be read.
require "./lib/png"

def usage() = "usage: pngmeta info FILE... | strip IN OUT | set IN OUT KEY VALUE"

# Reads and parses a file (structure only; see load_intact for CRCs).
def load(path: String) -> Array[Chunk] = parse_png(File.read(path))

# Every chunk's CRC must match before we rewrite a file: copying a
# damaged chunk into a fresh file would hide the damage.
def load_intact(path: String) -> Array[Chunk]
  chunks = load(path)
  damaged = chunks.reject() do |chunk| chunk.intact?() end
  unless damaged.empty?()
    raise PngError.new("#{damaged[0].kind()} chunk fails its CRC check; not rewriting a damaged file")
  end
  chunks
end

# Prints one file's summary and a line per chunk. Returns false if any chunk
# failed its CRC, so main can set the exit status; the file is still shown in
# full, with the bad chunk flagged.
def show_info(path: String) -> Bool
  chunks = load(path)
  header = header_of(chunks)
  interlace = if header.interlaced() then ", interlaced" else "" end
  puts("#{path}: #{header.width()}x#{header.height()}, #{header.bit_depth()}-bit #{header.color_name()}#{interlace}")

  intact = true

  chunks.each() do |chunk|
    # Flag a CRC mismatch, and add a human-readable detail for the chunk
    # kinds we understand (the later assignment wins for tIME).
    status = if chunk.intact?() then "" else "  CRC MISMATCH" end
    intact = false unless chunk.intact?()
    detail = ""
    entry = text_entry(chunk)
    detail = "  #{entry[0]}: #{entry[1]}" unless entry == nil
    detail = "  #{time_text(chunk)}" if chunk.kind() == "tIME"
    puts("  #{chunk.kind()} #{chunk.data().length().to_s().rjust(6, " ")}#{detail}#{status}")
  end
  intact
end

# `strip`: copy the file minus its metadata chunks. The pixel data (IDAT) is
# copied byte for byte, never decoded.
def strip(input: String, output: String)
  chunks = load_intact(input)
  kept = chunks.reject() do |chunk| metadata_kinds().include?(chunk.kind()) end
  written = File.write(output, encode_png(kept))
  puts("#{output}: #{written} bytes, removed #{chunks.length() - kept.length()} chunks")
end

# tEXt keywords are 1-79 printable Latin-1 characters; the text can't
# contain a NUL, since NUL separates the two.
def set_text(input: String, output: String, key: String, value: String)
  unless Regexp.new("^[\\x20-\\x7e]{1,79}$").match?(key)
    raise PngError.new("a keyword must be 1-79 printable ASCII characters")
  end
  raise PngError.new("the text can't contain a NUL byte") if value.include?("\0")

  # Load, dropping any existing entry with this keyword (that is what makes
  # `set` replace rather than duplicate).
  chunks = load_intact(input).reject() do |chunk|
    entry = text_entry(chunk)
    entry != nil && entry[0] == key
  end
  # Text chunks may go anywhere between IHDR and IEND; just before IEND
  # keeps the image data where it was.
  chunks.insert(chunks.length() - 1, make_chunk("tEXt", "#{key}\0#{value}"))
  written = File.write(output, encode_png(chunks))
  puts("#{output}: #{written} bytes, #{key} set")
end

def main(args: Array[String]) -> Int
  # A PngError (invalid or damaged file) is exit 1; an unreadable file, 66.
  begin
    case args
    # `info` takes any number of files; the exit status is 1 if ANY has a bad
    # CRC, but every file is still shown.
    when ["info", *paths]
      if paths.empty?()
        warn(usage())
        return 64
      end
      all_intact = true
      paths.each() do |path|
        all_intact = false unless show_info(path)
      end
      return if all_intact then 0 else 1 end
    when ["strip", input, output] then strip(input, output)
    when ["set", input, output, key, value] then set_text(input, output, key, value)
    else
      warn(usage())
      return 64
    end
    0
  rescue error: PngError
    warn("pngmeta: #{error.message()}")
    1
  rescue error: IOError
    warn("pngmeta: #{error.message()}")
    66
  end
end

exit(main(ARGV))
