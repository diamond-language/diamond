# A local read earlier in a statement than a block that captures it: the
# read must see the value, not the Cell the block's capture boxes it into.
def pair(a, b) = [a, b]
x = [1]
y = 5
literal = [x, [2].map() do |v| v end]
hash = {"y": y, "b": [2].map() do |v| v end}
call = pair(y, [3].map() do |v| v + y end)
sum = y + [2].map() do |v| v end.first()
spanning = [y,
  [3].map() do |v| v + y end]
[literal, hash, call, sum, spanning]
