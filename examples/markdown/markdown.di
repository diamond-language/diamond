# markdown: convert a Markdown subset to HTML.
#
#   diamond markdown.di [--toc] [--stats] [FILE]
#
# Reads FILE, or stdin when there is none. Supports headings, paragraphs,
# bullet and numbered lists, fenced code, blockquotes (nested), rules, and
# inline `code`, **strong**, *emphasis*, and [links](url).
#
#   --toc     start with a <nav> linking to each h2 and h3
#   --stats   print a word count and heading outline to stderr
require "./lib/render"

# All of the input as one string: stdin when `path` is nil, else the file.
def read_input(path) -> String
  if path == nil
    lines = []
    loop do
      line = gets()
      break if line == nil
      lines.push(line)
    end
    return lines.join("\n")   # gets() drops each line's newline
  end

  # `ensure` closes the file even if the read fails.
  file = File.open(path, "r")
  begin
    file.read()
  ensure
    file.close()
  end
end

# --stats: a word count, a tally of block kinds, and the heading outline,
# all to stderr so stdout stays pure HTML.
def stats(blocks: Array, text: String)
  # A word is a run of letters, digits and apostrophes.
  words = text.scan(Regexp.new("[A-Za-z0-9']+"))

  # Count blocks by class: map each to its class, then `tally` counts equal
  # ones.
  kinds = blocks.map() do |block| block.class() end.tally()
  warn("#{words.length()} words, #{blocks.length()} blocks: #{kinds.keys().sort().map() do |kind| "#{kind} #{kinds[kind]}" end.join(", ")}")

  # The outline: each heading indented by its level.
  blocks.each() do |block|
    if block is Heading
      warn("#{"  ".repeat(block.level() - 1)}#{block.text()}")
    end
  end
end

def markdown_main(args: Array) -> Int
  # Sort the arguments into the two known flags, file paths, and unknown
  # flags (anything starting with "--" that is not one of the known two).
  toc = args.include?("--toc")
  show_stats = args.include?("--stats")
  paths = args.reject() do |arg| arg.start_with?("--") end
  unknown = args.select() do |arg| arg.start_with?("--") && arg != "--toc" && arg != "--stats" end

  if !unknown.empty?() || paths.length() > 1
    warn("usage: markdown [--toc] [--stats] [FILE]")
    return 64
  end

  # `first_or(nil)`: the path if there is one, else nil (meaning stdin).
  text = nil
  begin
    text = read_input(paths.first_or(nil))
  rescue error: IOError
    warn("markdown: #{error.message()}")
    return 66
  end

  # Pipeline: parse into blocks, give headings their anchors (the table of
  # contents needs them), then render. `print` because the HTML already ends
  # each line with a newline.
  blocks = BlockParser.parse(text)
  assign_heading_ids(blocks)
  renderer = Renderer.new()
  print(renderer.toc(blocks)) if toc
  print(renderer.render(blocks))
  stats(blocks, text) if show_stats
  0
end

exit(markdown_main(ARGV))
