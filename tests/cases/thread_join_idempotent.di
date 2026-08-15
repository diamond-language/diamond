def worker()
  42
end
t = Thread.new(worker)
first = t.join()
second = t.join()
first == second
