# `Array * Int` used to rebuild the result with `result = result + values`
# once per repetition, copying everything so far each time: quadratic, so
# `[0] * 200_000` took minutes. These sizes finish instantly when it is
# linear and would time the test out if it ever regressed.
big = [7] * 100_000
puts(big.length())
puts(big[99_999])
pairs = [1, 2] * 50_000
puts(pairs.length())
puts(pairs[99_999])
puts(([] * 5).length())
puts(([1] * 0).length())
grid = (0...3).map() do |_| [0] * 4 end
grid[0][1] = 9
puts(grid.to_s())
