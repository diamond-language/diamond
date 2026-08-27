def double(value)
  value * 2
end

def array_large(values: Array, minimum)
  values.length() >= minimum
end

puts([1, 2].join(*[","]))
puts("hello".slice(*[1, 3]))
puts({"answer": 42}.fetch(*["answer", 0]))
puts([1, 2, 3].map(*[double]).join(","))
puts([1, 2, 3].large?(*[3]))
nil
