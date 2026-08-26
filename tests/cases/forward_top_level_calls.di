require "../multifile/forward_call_helper"

puts(later_value(prefix: "answer=", value: 42))
callable = doubled

def even(value: Int) -> Bool
  if value == 0 then true else odd(value - 1) end
end

def odd(value: Int) -> Bool
  if value == 0 then false else even(value - 1) end
end

def later_value[T](prefix: String, value: T) -> String
  "#{prefix}#{value}"
end

def doubled(value: Int) -> Int = value * 2

def later_from_main(value: Int) -> Int = value * 3

puts(callable(21))
[even(10), odd(9), call_main_function(7)]
