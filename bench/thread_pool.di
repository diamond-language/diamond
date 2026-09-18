# A pool of plain Thread workers, each doing a fixed, real amount of
# arithmetic on its own isolated heap, then joined -- the baseline this
# directory's own supervisor_pool.di compares Supervisor's steady-state
# (nothing ever crashes) overhead against. Exercises Thread.new/join
# spawn-and-teardown cost, not raw arithmetic throughput (int_arithmetic.di
# already covers that in-process). 5 workers, not more -- keeps this
# benchmark comfortably inside typical dev-machine headroom (leaves
# other cores free, doesn't contend for cache with everything else
# running locally) without changing what's actually being measured.
def worker(n)
  total = 0
  i = 0
  while i < n
    total = total + i
    i = i + 1
  end
  total
end

def run()
  threads = []
  index = 0
  while index < 5
    threads.push(Thread.new(worker, 50000))
    index = index + 1
  end
  total = 0
  threads.each() do |t|
    total = total + t.join()
  end
  total
end
run()
