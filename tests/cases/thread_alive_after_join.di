def worker()
  42
end
t = Thread.new(worker)
t.join()
t.alive?()
