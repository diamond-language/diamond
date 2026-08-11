module Hidden
  private

  def answer()
    1
  end
end

module Combined
  include Hidden
end

class Box
  include Combined
end

value = begin
  Box.new().answer()
rescue error
  42
end

puts(value)
