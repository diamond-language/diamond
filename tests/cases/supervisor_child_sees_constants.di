LIMIT = 30
SIZES = [10, 20]
PATTERN = Regexp.new("^[a-z]+$")

def reads_constants(results, attempts)
  attempts.send(1)
  raise "boom" if attempts.size() < 2
  results.send([LIMIT, SIZES, PATTERN.match?("abc")])
end

results = Channel.new(4)
attempts = Channel.new(10)
sup = Supervisor.new()
sup.add_child(reads_constants, results, attempts)
sup.join()
[results.receive(), sup.restart_count(0)]
