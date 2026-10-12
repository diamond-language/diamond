class Shape
end
class Circle < Shape
end
class Square < Shape
end
class Other
end
def area(shape: Circle | Square | Other)
  case shape
  when Shape
    1
  end
end
puts(area(Circle.new()))
