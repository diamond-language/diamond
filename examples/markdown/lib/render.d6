# Blocks to HTML, plus heading ids and an optional table of contents.
require "./blocks"

# Block list to HTML text.
class Renderer
  def initialize()
    # One Inline for the whole document (see its comment on regexp reuse).
    @inline = Inline.new()
  end

  # One HTML element per block, each on its own line.
  def render(blocks: Array) -> String
    html = StringBuilder.new()
    blocks.each() do |block| html.append(self.block(block)).append("\n") end
    html.to_s()
  end

  # <nav> with one link per h2/h3, indented by level.
  def toc(blocks: Array) -> String
    # Only levels 2 and 3 (level 1 is the page title).
    headings = blocks.select() do |block| block is Heading && block.level() >= 2 && block.level() <= 3 end
    return "" if headings.empty?()

    items = headings.map() do |heading|
      indent = "  ".repeat(heading.level() - 1)
      "#{indent}<li><a href=\"##{heading.id()}\">#{@inline.render(heading.text())}</a></li>"
    end
    "<nav>\n<ul>\n#{items.join("\n")}\n</ul>\n</nav>\n"
  end

  private

  # HTML for one block. Exhaustive over the sealed Block hierarchy.
  def block(block: Block) -> String
    case block
    when Heading
      "<h#{block.level()} id=\"#{block.id()}\">#{@inline.render(block.text())}</h#{block.level()}>"
    # A paragraph's lines are joined with spaces: Markdown treats a single
    # newline as ordinary whitespace.
    when Paragraph
      "<p>#{@inline.render(block.lines().map() do |line| line.strip() end.join(" "))}</p>"
    when ListBlock
      tag = if block.ordered?() then "ol" else "ul" end
      items = block.items().map() do |item| "  <li>#{@inline.render(item)}</li>" end
      "<#{tag}>\n#{items.join("\n")}\n</#{tag}>"
    # Code is escaped but never formatted; the language becomes a CSS class.
    when CodeBlock
      attribute = if block.language().empty?() then "" else " class=\"language-#{block.language()}\"" end
      "<pre><code#{attribute}>#{@inline.escape(block.lines().join("\n"))}</code></pre>"
    # A quote renders its inner blocks by calling `render` again.
    when Quote
      "<blockquote>\n#{self.render(block.blocks())}</blockquote>"
    when Rule
      "<hr>"
    end
  end
end

# "Hello, World!" -> "hello-world"; a repeated slug gets -2, -3, ...
def assign_heading_ids(blocks: Array)
  seen = {}
  non_word = Regexp.new("[^a-z0-9]+")
  edges = Regexp.new("^-+|-+$")
  headings = blocks.select() do |block| block is Heading end

  headings.each() do |heading|
    # Lowercase, collapse every run of other characters to one "-", and trim
    # dashes from both ends. A heading with no letters or digits at all gets
    # the placeholder "section".
    slug = heading.text().downcase().gsub(non_word, "-").gsub(edges, "")
    slug = "section" if slug.empty?()

    # Count uses of each slug so a repeated one is made unique (-2, -3, ...);
    # the first use stays plain.
    seen[slug] = seen.fetch(slug, 0) + 1
    heading.id = if seen[slug] == 1 then slug else "#{slug}-#{seen[slug]}" end
  end
end
