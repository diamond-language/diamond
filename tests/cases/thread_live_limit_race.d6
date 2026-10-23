# DIAMOND_MAX_LIVE_THREADS=6: three spawner threads then race to start children. Their own slots leave room for
# exactly three children, however the creations interleave; checking the cap and counting the thread later let
# simultaneous creators all pass the check and overshoot it.
def idle(hold) = hold.receive()

def spawner(go, results, hold)
  go.receive()
  made = 0
  attempts = 0
  while attempts < 6
    begin
      Thread.new(idle, hold)
      made += 1
    rescue error: ThreadError
      nil
    end
    attempts += 1
  end
  results.send(made)
  hold.receive()
end

go = Channel.new(8)
results = Channel.new(8)
hold = Channel.new(16)
spawners = []
3.times() do |round|
  spawners.push(Thread.new(spawner, go, results, hold))
end
3.times() do |round|
  go.send(true)
end
children = 0
3.times() do |round|
  children += results.receive()
end
(3 + children).times() do |round|
  hold.send(true)
end
spawners.each() do |spawner_thread|
  spawner_thread.join()
end
"children started: #{children}"
