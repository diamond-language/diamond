def consumer(ch, out)
  total = 0
  loop
    v = ch.receive()
    break if v == nil
    total = total + v
  end
  out.send(total)
end

a = Channel.new(2)
b = Channel.new(2)
out = Channel.new(2)
threads = [Thread.new(consumer, a, out), Thread.new(consumer, b, out)]

# One producer multiplexes two bounded channels; backpressure forces it to
# park until either consumer makes room.
sent_to = [0, 0]
i = 1
while i <= 100
  index = Channel.select_send([a, b], i)
  sent_to[index] = sent_to[index] + 1
  i = i + 1
end
a.send(nil)
b.send(nil)
threads.each() do |t|
  t.join()
end
[out.receive() + out.receive(), sent_to[0] + sent_to[1]]
