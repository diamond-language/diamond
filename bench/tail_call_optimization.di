# Self-recursive tail-call optimization (docs/callables.md) -- a
# function's own tail call to itself reuses the current run_chunk
# activation in place (O(1) native stack) instead of a real recursive
# re-entry, so this qualifying shape (an explicit `return` guard clause
# followed by `return selfcall(...)` as the function's own trailing
# statement -- see docs/callables.md's "What qualifies") runs 3,000,000
# levels deep without ever approaching DIAMOND_MAX_CALL_DEPTH (95).
# Confirms the optimization is actually active, not just "didn't crash
# by luck": ordinary (non-tail, or non-self, or generic/variadic)
# recursion this deep would overflow at depth 95, long before this
# function's own base case is ever reached.
def sum_recursive(n, acc)
  return acc if n == 0
  return sum_recursive(n - 1, acc + n)
end

def run()
  sum_recursive(3000000, 0)
end
run()
