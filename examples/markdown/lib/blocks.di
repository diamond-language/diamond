# Block structure: the document as a list of Blocks. Block is sealed, so
# the renderer's `case` must handle every kind.
require "./inline"

sealed class Block
end

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

class Paragraph < Block
  attr_reader lines: Array[String]
  def initialize(lines: Array[String])
    @lines = lines
  end
end

class ListBlock < Block
  attr_predicate ordered: Bool
  attr_reader items: Array[String]
  def initialize(ordered: Bool, items: Array[String])
    @ordered = ordered
    @items = items
  end
end

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

class Rule < Block
end

class BlockParser
  def initialize(lines: Array[String])
    @lines = lines
    @at = 0
    @heading = Regexp.new("^(#+) +(.*?) *#*$")
    @rule = Regexp.new("^ *([-*_] *){3,}$")
    @fence = Regexp.new("^```(.*)$")
    @quote = Regexp.new("^> ?(.*)$")
    @bullet = Regexp.new("^[-*+] +(.*)$")
    @numbered = Regexp.new("^[0-9]+[.)] +(.*)$")
    @blank = Regexp.new("^\\s*$")
  end

  def self.parse(text: String) -> Array
    BlockParser.new(text.split("\n")).blocks()
  end

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
    case line
    when @blank
      @at += 1
      nil
    when @fence
      language = @fence.match(line)[1].strip()
      @at += 1
      code = self.take_while() do |text| !text.start_with?("```") end
      @at += 1   # the closing fence, if there is one
      CodeBlock.new(language, code)
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
    when @quote
      inner = self.take_while() do |text| @quote.match?(text) end
      Quote.new(BlockParser.new(inner.map() do |text| @quote.match(text)[1] end).blocks())
    when @bullet
      ListBlock.new(false, self.list_items(@bullet))
    when @numbered
      ListBlock.new(true, self.list_items(@numbered))
    else
      Paragraph.new(self.paragraph_lines())
    end
  end

  # Consumes lines while `keep` says so; returns them.
  def take_while(&keep) -> Array[String]
    taken = []
    while @at < @lines.length() && yield(@lines[@at])
      taken.push(@lines[@at])
      @at += 1
    end
    taken
  end

  def list_items(marker) -> Array[String]
    items = self.take_while() do |text| marker.match?(text) end
    items.map() do |text| marker.match(text)[1] end
  end

  # A paragraph runs until a blank line or the start of any other block.
  def paragraph_lines() -> Array[String]
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
