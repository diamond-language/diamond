# Block structure: the document as a list of Blocks. Block is sealed, so
# the renderer's `case` must handle every kind.
require "./inline"

sealed class Block
end

# One class per kind of block.

# `#`-style heading, level 1-6. `id` (the anchor slug) is filled in later by
# assign_heading_ids once all headings are known, since a duplicate title
# needs the others to be seen first.
class Heading < Block
  attr_reader level: Int
  attr_reader text: String
  attr_accessor id: String
  def initialize(level: Int, text: String)
    @level = level
    @text = text
    @id = ""
  end
end

# Consecutive lines of text, kept separate so the renderer decides how to
# join them.
class Paragraph < Block
  attr_reader lines: Array[String]
  def initialize(lines: Array[String])
    @lines = lines
  end
end

# A bullet or numbered list. Items are raw text (inline markup is rendered
# later). There is no nesting.
class ListBlock < Block
  attr_predicate ordered: Bool
  attr_reader items: Array[String]
  def initialize(ordered: Bool, items: Array[String])
    @ordered = ordered
    @items = items
  end
end

# A ``` fenced block. `language` is whatever follows the opening fence.
class CodeBlock < Block
  attr_reader language: String
  attr_reader lines: Array[String]
  def initialize(language: String, lines: Array[String])
    @language = language
    @lines = lines
  end
end

# A blockquote holds blocks of its own, parsed recursively.
class Quote < Block
  attr_reader blocks: Array
  def initialize(blocks: Array)
    @blocks = blocks
  end
end

# A horizontal rule (---, ***, ___).
class Rule < Block
end

# Reads lines top to bottom, deciding at each position what block starts
# there. `@at` is the next unread line; every helper advances it past what
# it consumed. The patterns are compiled once, here.
class BlockParser
  def initialize(lines: Array[String])
    @lines = lines
    @at = 0

    # Block-start patterns. The heading pattern's `(.*?) *#*$` drops
    # trailing spaces and an optional closing run of #s from the text. The
    # rule allows spaces between its three or more markers.
    @heading = Regexp.new("^(#+) +(.*?) *#*$")
    @rule = Regexp.new("^ *([-*_] *){3,}$")
    @fence = Regexp.new("^```(.*)$")
    @quote = Regexp.new("^> ?(.*)$")
    @bullet = Regexp.new("^[-*+] +(.*)$")
    @numbered = Regexp.new("^[0-9]+[.)] +(.*)$")
    @blank = Regexp.new("^\\s*$")
  end

  # Entry point: Markdown text to a list of Blocks.
  def self.parse(text: String) -> Array
    BlockParser.new(text.split("\n")).blocks()
  end

  # Repeatedly take the next block until the lines run out (blank lines
  # yield nil and are dropped).
  def blocks() -> Array
    blocks = []

    while @at < @lines.length()
      block = self.next_block()
      blocks.push(block) if block != nil
    end
    blocks
  end

  private

  # Returns the block starting at the current line, or nil for a blank line.
  def next_block()
    line = @lines[@at]

    # `case line` with a Regexp in each `when` tests whether the line matches
    # it. ORDER MATTERS: more specific shapes come before the catch-all
    # paragraph, and the rule must precede the bullet list (a line like
    # "- - -" matches both, and means a rule).
    case line
    when @blank
      @at += 1
      nil
    # Fenced code: the text after the opening ``` is the language, then
    # everything up to the closing fence is code, kept verbatim.
    when @fence
      language = @fence.match(line)[1].strip()
      @at += 1
      code = self.take_while() do |text| !text.start_with?("```") end
      @at += 1   # the closing fence, if there is one
      CodeBlock.new(language, code)
    # Heading: the number of #s is the level. Seven or more is not a heading,
    # so such a line is treated as ordinary paragraph text.
    when @heading
      [_, hashes, text] = @heading.match(line)
      if hashes.length() > 6
        Paragraph.new(self.paragraph_lines())
      else
        @at += 1
        Heading.new(hashes.length(), text)
      end
    when @rule
      @at += 1
      Rule.new()
    # Blockquote: gather every consecutive "> ..." line, strip the marker,
    # and parse the remainder as a document of its own (so quotes can hold
    # lists, code, and further quotes).
    when @quote
      inner = self.take_while() do |text| @quote.match?(text) end
      Quote.new(BlockParser.new(inner.map() do |text| @quote.match(text)[1] end).blocks())
    # Lists: one block per run of consecutive items of the same kind.
    when @bullet
      ListBlock.new(false, self.list_items(@bullet))
    when @numbered
      ListBlock.new(true, self.list_items(@numbered))
    # Anything else is paragraph text.
    else
      Paragraph.new(self.paragraph_lines())
    end
  end

  # Consumes lines while `keep` says so; returns them. `&keep` receives the
  # caller's block, and `yield(...)` calls it.

  def take_while(&keep) -> Array[String]
    taken = []
    while @at < @lines.length() && yield(@lines[@at])
      taken.push(@lines[@at])
      @at += 1
    end
    taken
  end

  # The text of each item in a run of lines matching `marker`, with the
  # bullet or number removed (capture group 1).
  def list_items(marker) -> Array[String]
    items = self.take_while() do |text| marker.match?(text) end
    items.map() do |text| marker.match(text)[1] end
  end

  # A paragraph runs until a blank line or the start of any other block.
  def paragraph_lines() -> Array[String]
    # The first line is already known to belong; later lines are taken until
    # one of them would start a different block.
    lines = [@lines[@at]]
    @at += 1
    more = self.take_while() do |text|
      ![@blank, @fence, @heading, @rule, @quote, @bullet, @numbered].any?() do |pattern|
        pattern.match?(text)
      end
    end
    lines.concat(more)
  end
end
