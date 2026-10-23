# tap yields self to the block and returns self (not the block's own
# result), on any receiver kind -- a primitive, a native collection, or
# an Instance.
seen = []
result = 5.tap() do |x|
  seen.push(x)
end
array_result = [1, 2, 3].tap() do |arr|
  seen.push(arr.length())
end
"#{result}, #{array_result}, #{seen}"
