# A thread whose budget trips, rescues the error and keeps running is stopped by its
# own hard limit; the parent sees it as a ThreadError from join and carries on.
def worker()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    nil
  end
  while true
    index = index + 1
  end
end
t = Thread.new(worker)
begin
  t.join()
rescue e
  puts("join raised #{e.class()}: #{e.message()}")
end
puts("main continues")
