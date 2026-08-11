module Values
  def value() -> Int
    54
  end
end

class Box
  include Values
end

puts(Box.new().value())
