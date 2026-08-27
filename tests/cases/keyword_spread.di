puts(format_values(*["left"], middle: "middle", right: "right"))

def format_values(left, middle, right = "default")
  [left, middle, right].join(":")
end

def typed_pair[T](first: T, second: T)
  [first, second]
end

def collect(head, *rest)
  head + ":" + rest.join(",")
end

puts(format_values("a", *["b"], right: "c"))
puts(typed_pair[Int](*[10], second: 20).join(","))
puts(collect(*["head"], rest: "tail"))
nil
