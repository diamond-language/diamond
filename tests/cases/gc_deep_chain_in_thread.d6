# The same deep-chain marking, on a worker Thread: a Thread's native stack is
# smaller than the main thread's, so recursive marking would fail here sooner.
def build_and_walk(depth)
  chain = []
  depth.times() do |i|
    chain = [chain]
  end
  walked = 0
  cursor = chain
  while cursor.length() > 0
    cursor = cursor[0]
    walked += 1
  end
  walked
end

thread = Thread.new(build_and_walk, 150000)
puts("thread chain: #{thread.join()}")
