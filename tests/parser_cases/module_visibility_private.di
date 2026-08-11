module Hidden
  private
  def answer()
    1
  end
end

class Box
  include Hidden
end

value = begin
  Box.new().answer()
rescue error
  42
end
puts(value)
