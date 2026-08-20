# A named escape hatch from `result = result + piece` in a loop -- the
# O(n^2) concatenation pattern the pre-release audit flagged, and the
# same reason #join above is a genuine native, O(n) method rather than
# a Diamond-level loop. StringBuilder itself stays a thin Array
# wrapper rather than its own native object: #push is already O(1)
# amortized (realloc-doubling) and #join is now O(n) total, so
# accumulating pieces in an Array and joining once at the end already
# has the right complexity -- this class just gives that pattern an
# obvious name. Diamond has no `<<` operator (see docs/syntax.md's
# operator-overloading list), so #append is a plain method, not `<<`.
class StringBuilder
  def initialize()
    @parts = []
    @total_length = 0
  end

  def append(piece)
    text = "#{piece}"
    @parts.push(text)
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
