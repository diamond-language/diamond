begin
  1
rescue error: Int | Float | String | Bool | Nil | Array | Hash | Callable | Object
  error
end
