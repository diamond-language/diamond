# Channel send/receive round-trip cost between two real OS threads --
# every payload is deep-copied across the heap boundary (docs/threads.md),
# so this measures that copy plus the bounded queue's own thread-safe
# synchronization, not raw arithmetic. No prior bench/*.di coverage
# existed for Channel/Supervisor/Thread at all before this file.
def producer(ch)
  i = 0
  while i < 20000
    ch.send(i)
    i = i + 1
  end
  ch.close()
end

def run()
  ch = Channel.new(64)
  worker = Thread.new(producer, ch)
  total = 0
  loop
    item = ch.receive()
    break if item == nil
    total = total + item
  end
  worker.join()
  total
end
run()
