def worker() = 42
t = Thread.new(worker)
first = t.join()
second = t.join()
first == second
