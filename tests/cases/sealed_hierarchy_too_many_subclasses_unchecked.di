sealed class Shape
end
class S0 < Shape
end
class S1 < Shape
end
class S2 < Shape
end
class S3 < Shape
end
class S4 < Shape
end
class S5 < Shape
end
class S6 < Shape
end
class S7 < Shape
end
class S8 < Shape
end
def f(shape: Shape)
  case shape
  when S0
    1
  end
end
puts("compiled ok")
