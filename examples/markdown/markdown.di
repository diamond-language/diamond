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
  file = File.open(path, "r")
  begin
    file.read()
  ensure
    file.close()
  end
end

def stats(blocks: Array, text: String)
  words = text.scan(Regexp.new("[A-Za-z0-9']+"))
  kinds = blocks.map() do |block| block.class() end.tally()
  warn("#{words.length()} words, #{blocks.length()} blocks: #{kinds.keys().sort().map() do |kind| "#{kind} #{kinds[kind]}" end.join(", ")}")
  blocks.each() do |block|
    if block is Heading
      warn("#{"  ".repeat(block.level() - 1)}#{block.text()}")
    end
  end
end

def markdown_main(args: Array) -> Int
  toc = args.include?("--toc")
  show_stats = args.include?("--stats")
  paths = args.reject() do |arg| arg.start_with?("--") end
  unknown = args.select() do |arg| arg.start_with?("--") && arg != "--toc" && arg != "--stats" end
  if !unknown.empty?() || paths.length() > 1
    warn("usage: markdown [--toc] [--stats] [FILE]")
    return 64
  end
  text = nil
  begin
    text = read_input(paths.first_or(nil))
  rescue error: IOError
    warn("markdown: #{error.message()}")
    return 66
  end
  blocks = BlockParser.parse(text)
  assign_heading_ids(blocks)
  renderer = Renderer.new()
  print(renderer.toc(blocks)) if toc
  print(renderer.render(blocks))
  stats(blocks, text) if show_stats
  0
end

exit(markdown_main(ARGV))
