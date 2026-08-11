module Values
  ANSWER = 55

  def value() -> Int
    ANSWER
  end
end

class Box
  include Values
end

puts(Box.new().value())
