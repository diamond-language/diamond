def late()
  LATER
end

worker = Thread.new(late)
LATER = 1
begin
  worker.join()
  "joined"
rescue error: StandardError
  error.message().include?("uninitialized constant")
end
