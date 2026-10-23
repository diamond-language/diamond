module Mixed
  def first()
    1
  end

  def second()
    2
  end

  private first, second
  public second
end

class Box
  include Mixed
end

first = begin
  Box.new().first()
rescue error
  40
end

puts(first + Box.new().second())
