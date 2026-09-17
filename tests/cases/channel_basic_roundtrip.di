ch = Channel.new(4)
ch.send(1)
ch.send("two")
ch.send([3, 3, 3])
first = ch.receive()
second = ch.receive()
third = ch.receive()
[first, second, third.length(), ch.size()]
