errors = 0
[[nil, 1], [1, 1], ["127.0.0.1", "bad"], ["", 1], ["localhost", 1], ["[::1]", 1], ["::1%lo0", 1], ["127.0.0.1" + chr(0) + "ignored", 1], ["127.0.0.1", -1], ["127.0.0.1", 0], ["127.0.0.1", 65536]].each() do |args|
  begin
    TCPSocket.connect_nonblocking(args[0], args[1])
    raise "invalid address or port accepted"
  rescue error: TypeError
    errors += 1
  end
end
puts(errors)
