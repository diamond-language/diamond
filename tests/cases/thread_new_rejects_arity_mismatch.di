def worker(x, y)
  x + y
end
Thread.new(worker, 1)
