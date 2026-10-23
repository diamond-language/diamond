module Mixed
  def hidden()
    42
  end

  private hidden
  public hidden
end

class Box
  include Mixed
end

puts(Box.new().hidden())
