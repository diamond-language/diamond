def worker(x, y)
  x + y
end
t = Thread.new(worker, 3, 4)
t.join()
