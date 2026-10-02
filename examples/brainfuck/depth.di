# depth: which recursion shapes get tail-call optimization, measured.
#
# depth.di [N]     recurse N deep (default 100000) in each shape
#
# Only a function's own call to itself, as the whole value of a `return`
# (or as the body's trailing expression), runs in constant stack. Everything
# else is ordinary recursion, limited to a fixed depth of 95 calls in all,
# counting the frames the caller already uses. None of the failing shapes
# is an error in the language; they just hit that limit.

# Each function below counts down from n to 0 in a different SHAPE, and
# `attempt` reports whether it survives n levels of recursion.
#
# The ideal: the recursive call is the whole value of a `return`, and the
# running total travels in `acc`, so nothing is left to do after the call
# returns. That is a tail call, and runs in constant stack.
def count_tail(n: Int, acc: Int) -> Int
  return acc if n == 0
  return count_tail(n - 1, acc + 1)
end

# Same computation, but work is done to the result, so it is not a tail call.
def count_plain(n: Int) -> Int
  return 0 if n == 0
  1 + count_plain(n - 1)
end

# A tail call, but inside begin/rescue: a live handler's resume point is
# tied to the current frame, so this shape is disqualified.
def count_guarded(n: Int, acc: Int) -> Int
  return acc if n == 0
  begin
    return count_guarded(n - 1, acc + 1)
  rescue error: ArgumentError
    return -1
  end
end

# Tail calls, but to a different function each time: only self-recursion counts.
def ping(n: Int, acc: Int) -> Int
  return acc if n == 0
  return pong(n - 1, acc + 1)
end

def pong(n: Int, acc: Int) -> Int
  return acc if n == 0
  return ping(n - 1, acc + 1)
end

# Runs `work` (a zero-argument function) and prints one table row: the
# shape, the depth, and whether the answer was right, wrong, or the stack
# overflowed. SystemStackError is the "too deep" failure, and is rescued so
# the demo can continue to the next shape.
def attempt(label: String, depth: Int, expected: Int, work)
  result = begin
    value = work()
    if value == expected then "ok" else "WRONG: #{value}" end
  rescue error: SystemStackError
    "stack overflow"
  end
  puts("#{label.ljust(34, " ")} #{depth.to_s().rjust(8, " ")}   #{result}")
end

def main(argv) -> Int
  n = if argv.empty?() then 100000 else argv[0].to_i() end
  if n < 1
    warn("usage: depth.di [N]")
    return 64
  end
  puts("#{"shape".ljust(34, " ")} #{"depth".rjust(8, " ")}   result")

  # `attempt` takes a function value, so each shape is wrapped in a
  # zero-argument nested def that captures n.
  def tail() = count_tail(n, 0)
  def plain() = count_plain(n)
  def guarded() = count_guarded(n, 0)
  def mutual() = ping(n, 0)
  def small() = count_plain(80)

  # Expected: the self tail call passes at any depth; the three that are not
  # self tail calls overflow at the large depth; and plain recursion is fine
  # when it stays within the fixed limit (80 is under it).
  attempt("self tail call", n, n, tail)
  attempt("non-tail: 1 + f(n - 1)", n, n, plain)
  attempt("tail call inside begin/rescue", n, n, guarded)
  attempt("mutual tail calls (ping/pong)", n, n, mutual)
  attempt("non-tail, within the limit", 80, 80, small)
  0
end

exit(main(ARGV))
