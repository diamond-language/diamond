def checked(values: Hash[String, Int | Nil]) -> Hash[String, Int | Nil]
  values
end

values = {"answer": 40, "optional": nil}
checked(values)

key_guard = begin
  values[1] = 1
  0
rescue error: TypeError
  1
end

value_guard = begin
  values["answer"] = "bad"
  0
rescue error: TypeError
  1
end

values["optional"] = 2

inner = [1]
def nested(values: Hash[String, Array[Int]])
  values
end
nested({"inner": inner})
nested_guard = begin
  inner[0] = "bad"
  0
rescue error: TypeError
  1
end

values["answer"] + values["optional"] + key_guard + value_guard + nested_guard - 3
