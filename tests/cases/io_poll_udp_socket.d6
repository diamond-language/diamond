# IO.poll accepts a UDPSocket: it is not readable until a datagram is
# queued, then readable, so a following receive() cannot block. Also a
# writable check, a closed-socket error, and a rejected non-pollable value.
server = UDPSocket.bind(47831)
client = UDPSocket.open()
puts(IO.poll([server], [], 0)["readable"][0])
client.send("ping", "127.0.0.1", 47831)
puts(IO.poll([server], [], 2000)["readable"][0])
got = server.receive(64)
puts(got["data"])
puts(IO.poll([server], [], 0)["readable"][0])
puts(IO.poll([], [client], 0)["writable"][0])
server.send("pong", got["host"], got["port"])
ready = IO.poll([server, client], [], 2000)["readable"]
puts("#{ready[0]} #{ready[1]}")
puts(client.receive(64)["data"])
server.close()
begin
  IO.poll([server], [], 0)
rescue error: IOError
  puts(error.message())
end
client.close()
begin
  IO.poll([5], [], 0)
rescue error: TypeError
  puts("rejected")
end
nil
