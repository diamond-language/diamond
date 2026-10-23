def checked(values: Array[Int | Nil]) -> Array[Int | Nil] = values

class Element
end
class ChildElement < Element
end
def nominal(values: Array[Element]) = values
nominal([ChildElement.new()])

values = [40, nil]
checked(values)
mutation = begin
  values[1] = "bad"
  0
rescue error: TypeError
  values[1] = 2
end

inner = [1]
def nested(values: Array[Array[Int]]) = values
nested([inner])
nested_guard = begin
  inner[0] = "bad"
  0
rescue error: TypeError
  1
end

values[0] + values[1] + mutation + nested_guard - 3
