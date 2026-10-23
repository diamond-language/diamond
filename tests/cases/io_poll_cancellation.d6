def cancel_when_ready(channel, ready)
  ready.receive()
  channel.close()
end
cancel = Channel.new(1)
ready = Channel.new(1)
thread = Thread.new(cancel_when_ready, cancel, ready)
ready.send(true)
started = Time.monotonic()
result = IO.poll([], [], {"cancellations": [cancel, cancel], "deadline": started + 10.0})
woke = Time.monotonic() - started < 5.0
thread.join()
IO.poll([], [], {"cancellations": [cancel]})
started = Time.monotonic()
IO.poll([], [], {"cancellations": [], "deadline": started + 0.02})
expired = Time.monotonic() >= started + 0.02
socket = UDPSocket.open()
ready = IO.poll([], [socket, socket], {"cancellations": [], "deadline": Time.monotonic() + 1.0})
socket.close()
errors = 0
[{}, {"cancellations": [1]}, {"cancellations": [], "deadline": "bad"}, {"cancellations": [], "typo": 1}].each() do |options|
  begin
    IO.poll([], [], options)
  rescue error: TypeError
    errors += 1
  end
end
[woke, result["readable"], expired, ready["writable"], errors]
