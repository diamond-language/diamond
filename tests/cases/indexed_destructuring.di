values = [0, 0]
head, values[1] = "head", 9
puts(head)
puts(values.join(","))

matrix = [[0, 0], [0, 0]]
[matrix[0][1], matrix[1][0]] = [7, 8]
puts(matrix[0].join(","))
puts(matrix[1].join(","))

keys = ["left", "right"]
result = {"left": 0, "right": 0}
{"first": result[keys[0]], "second": result[keys[1]]} = {"first": 11, "second": 12}
puts(result["left"])
puts(result["right"])

atomic = [0]
begin
  [atomic[0], [inner_left, inner_right]] = [99, [1]]
rescue error: ArgumentError
  puts(error.message())
end
puts(atomic[0])
