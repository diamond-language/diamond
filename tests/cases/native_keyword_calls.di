def double(value)
  value * 2
end

puts([1, 2].join(separator: ","))
puts("hello".slice(start: 1, length: 3))
puts("a,b".split(separator: ",").join("-"))
puts({"answer": 42}.fetch(key: "answer", fallback: 0))
puts([1, 2, 3].map(callback: double).join(","))
puts([1, 2, 3].take(n: 2).join(","))
puts("x".ljust(width: 3, padding: "."))
nil
