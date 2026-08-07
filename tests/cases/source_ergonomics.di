# Diamond source should remain readable as examples grow.
class Pair
  def initialize(left, right,)
    @values = [left, right,] # trailing array comma
  end

  def sum()
    @values[0] + @values[1]
  end
end

pair = Pair.new(20, 22,); ignored = 1_000_000
pair.sum() # final value
