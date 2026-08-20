# A small escape hatch from repeated string concatenation. Parts are collected
# with Array's native `<<` push operation and joined once in `to_s()`, keeping
# append-heavy callers linear without adding another native object type.
class StringBuilder
  def initialize()
    @parts = []
    @total_length = 0
  end

  def append(piece)
    text = "#{piece}"
    @parts << text
    @total_length = @total_length + text.length()
    self
  end

  def length() -> Int
    @total_length
  end

  def to_s() -> String
    @parts.join("")
  end
end
