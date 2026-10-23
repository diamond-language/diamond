require "../lib/cancellation"

def cancel_later(source, gate)
  gate.receive()
  Cancellation::Source.new().token().sleep(0.05)
  source.cancel()
end

def run(port, mode)
  parent = Cancellation::Source.new()
  source = Cancellation::Source.new(parent.token())
  token = source.token()
  def stop()
    parent.cancel()
  end
  if mode == "signal" then Signal.trap("TERM", stop) end
  listener = TCPServer.listen_nonblocking(port)
  socket = nil
  worker = nil
  gate = Channel.new(1)
  puts("ready")
  begin
    loop do
      # The harness bounds setup; the tested deadline starts after accept.
      IO.poll([listener], [], 1000)
      socket = listener.accept()
      break if socket != nil
    end
    socket.finish_connect()
    if mode == "deadline"
      parent = Cancellation::Source.new(nil, 0.05)
      source = Cancellation::Source.new(parent.token(), 10)
      token = source.token()
    end
    if mode == "read" || mode == "write"
      worker = Thread.new(cancel_later, parent, gate)
    end
    if mode == "write"
      chunk = "x".repeat(65536)
      begin
        loop do socket.write(chunk) end
      rescue error: WouldBlockError
        puts("blocked")
      end
      gate.send(true)
      token.write(socket, "x".repeat(8388608))
      raise "blocked write was not cancelled"
    elsif mode == "success"
      received = ""
      loop do
        chunk = token.read(socket, 3)
        break if chunk == nil
        received = received + chunk
      end
      unless received == "abc" then raise "read data lost" end
      written = token.write(socket, "abc".repeat(1048576))
      unless written == 3145728 then raise "partial write lost" end
      puts("success")
    else
      puts("waiting")
      if mode == "read" then gate.send(true) end
      token.read(socket, 1)
      raise "stalled read was not cancelled"
    end
  rescue error: Cancellation::DeadlineExceeded
    puts("deadline")
  rescue error: Cancellation::Cancelled
    puts("cancelled")
  ensure
    if socket != nil then socket.close() end
    listener.close()
    if worker != nil then worker.join() end
    puts("cleaned")
  end
end
run(ARGV[0].to_i(), ARGV[1])
exit(0)
