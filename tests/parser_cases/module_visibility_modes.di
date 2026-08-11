module Mixed
  private
  def hidden()
    1
  end

  public
  def visible()
    42
  end
end

class Box
  include Mixed
end

puts(Box.new().visible())
