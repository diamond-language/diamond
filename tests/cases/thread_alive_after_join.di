def worker() = 42
t = Thread.new(worker)
t.join()
t.alive?()
