# Weak-scaling check: every thread does the same fixed amount of CPU work,
# so perfect scaling keeps wall time flat as the thread count grows.
# Usage: diamond thread_scaling.di THREADS ITERATIONS_PER_THREAD
def spin(iterations: Int) -> Int
  total = 0
  i = 0
  while i < iterations
    total = (total + i * 7) % 1000003
    i += 1
  end
  total
end

count = ARGV[0].to_i()
per_thread = ARGV[1].to_i()
workers = []
index = 0
while index < count
  workers.push(Thread.new(spin, per_thread))
  index += 1
end
checksum = 0
workers.each() do |worker|
  checksum = (checksum + worker.join()) % 1000003
end
checksum
