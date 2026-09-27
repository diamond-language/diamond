# Array + Array (add_fallback, src/vm.c) used to build a throwaway buffer
# with both arrays' elements and hand it to allocate_array, which copied
# it a second time into the result's own storage. Rewritten to memcpy
# straight into the new array. Exercises both halves, an empty side on
# either end, both empty, and that the two operands are left untouched.
a = [1, 2, 3]
b = [4, 5]
puts((a + b).to_s())
puts((b + a).to_s())
puts(([] + b).to_s())
puts((a + []).to_s())
puts(([] + []).to_s())
puts(a.to_s())
puts(b.to_s())

# A right-hand array large enough that a naive off-by-one in the second
# memcpy's destination offset would show up as garbage or a crash.
big = (0...5000).map() do |i| i end
joined = a + big
puts(joined.length())
puts(joined[0])
puts(joined[3])
puts(joined[3 + 4999])
