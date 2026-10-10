ITEMS = [1]

def read_snapshot(start, results)
  start.receive()
  missing = begin
    LATER
    false
  rescue error: StandardError
    error.message().include?("uninitialized constant")
  end
  ITEMS.push(3)
  results.send([ITEMS, missing])
end

start = Channel.new(1)
results = Channel.new(1)
sup = Supervisor.new()
sup.add_child(read_snapshot, start, results)
ITEMS.push(2)
LATER = 42
start.send(true)
sup.join()
[results.receive(), ITEMS]
