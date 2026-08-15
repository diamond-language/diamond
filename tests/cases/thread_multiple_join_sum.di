def square(n)
  n * n
end
t1 = Thread.new(square, 3)
t2 = Thread.new(square, 4)
t3 = Thread.new(square, 5)
t1.join() + t2.join() + t3.join()
