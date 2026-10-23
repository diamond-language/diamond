# flat_map flattens exactly one level -- a non-Array block result is
# pushed directly, not an error.
nested = [1, 2, 3].flat_map() do |x|
  [x, x * 10]
end
scalar = [1, 2, 3].flat_map() do |x|
  x * 2
end
"#{nested}, #{scalar}"
