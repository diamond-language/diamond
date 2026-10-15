# MySQL.open waiting for a server that accepts the connection but never sends
# its greeting is interrupted by Supervisor#stop(timeout_ms) instead of
# keeping stop() waiting.
def opener()
  begin
    MySQL.open("127.0.0.1", "u", "p", "d", 19737)
  rescue error: StandardError
    nil
  end
end

listener = TCPServer.listen_nonblocking(19737)
sup = Supervisor.new()
sup.add_child(opener)
IO.poll([listener], [], 5000)
client = listener.accept()
sup.stop(300)
puts(sup.alive?(0))
client.close()
listener.close()
