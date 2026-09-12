def consumer(ch)
  total = 0
  loop
    value = ch.receive()
    break if value == nil
    total = total + value
  end
  total
end

ch = Channel.new(2)
t = Thread.new(consumer, ch)
i = 0
while i < 20
  ch.send(i)
  i = i + 1
end
ch.close()
t.join()
