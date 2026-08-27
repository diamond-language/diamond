class Pair
  def values(first, second)
    [first, second]
  end
end

Pair.new().values(*[1, 2], second: 3)
