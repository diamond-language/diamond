def worker()
  [1, 2, 3]
end
t = Thread.new(worker)
t.join()
