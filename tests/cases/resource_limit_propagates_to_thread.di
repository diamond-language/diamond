def worker()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    "caught"
  end
end
t = Thread.new(worker)
t.join()
