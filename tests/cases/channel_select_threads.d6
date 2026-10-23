def producer(ch, tag, count)
  count.times() do |i|
    ch.send([tag, i])
  end
  ch.close()
end

a = Channel.new(2)
b = Channel.new(2)
c = Channel.new(2)
threads = [Thread.new(producer, a, 0, 50),
           Thread.new(producer, b, 1, 30),
           Thread.new(producer, c, 2, 20)]

# One consumer multiplexes all three bounded channels; backpressure
# forces the producers to interleave. Select returns nil only once
# every channel is closed and drained.
counts = [0, 0, 0]
in_order = true
last = [-1, -1, -1]
loop
  picked = Channel.select([a, b, c])
  break if picked == nil
  tag = picked[1][0]
  index = picked[1][1]
  counts[tag] = counts[tag] + 1
  in_order = false if index != last[tag] + 1
  last[tag] = index
end
threads.each() do |t|
  t.join()
end
counts + [in_order]
