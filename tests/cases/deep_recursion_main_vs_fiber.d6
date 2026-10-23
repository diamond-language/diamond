# Ordinary recursion runs on the main program's large stack: thousands of
# levels complete. A fiber has its own small stack, so the same recursion
# inside one stops with a clean stack-overflow error rather than crashing.
def depth(n)
  return 0 if n == 0
  return 1 + depth(n - 1)
end

puts(depth(3000))

def make_worker()
  def worker() = depth(3000)
  worker
end

fiber = Fiber.new(make_worker())
puts(fiber.resume())
