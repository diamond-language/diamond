require "../lib/cancellation"

def cancel_later(source)
  Cancellation::Source.new().token().sleep(0.08)
  source.cancel()
end

def run(address, port, mode)
  source = Cancellation::Source.new(nil, if mode == "deadline" then 0.08 else nil end)
  token = Cancellation::Source.new(source.token()).token()
  socket = nil
  worker = nil
  def cancel() = source.cancel()
  if mode == "signal" then Signal.trap("TERM", cancel) end
  if mode == "pre" then source.cancel() end
  begin
    if mode == "refused" || mode == "leaks"
      if mode == "leaks" then puts("baseline"); gets() end
      100.times() do |i|
        begin
          socket = TCPSocket.connect_nonblocking(address, port)
          loop do
            begin
              socket.finish_connect()
              raise "refused connection reported success"
            rescue error: WouldBlockError
              IO.poll([], [socket], 100)
            end
          end
        rescue error: IOError
          if socket != nil
            begin
              socket.finish_connect()
              raise "failed connection resurrected"
            rescue error: IOError
              nil
            end
          end
        end
      end
      puts("refused")
      if mode == "leaks" then gets() end
    elsif mode == "cancel_leaks"
      puts("baseline")
      gets()
      50.times() do |i|
        begin
          Cancellation::Source.new(nil, 0.002).token().connect(address, port)
          raise "stalled connection returned"
        rescue error: Cancellation::DeadlineExceeded
          nil
        end
      end
      puts("cancelled repeatedly")
      gets()
    elsif mode == "pending"
      socket = TCPSocket.connect_nonblocking(address, port)
      errors = 0
      begin
        socket.finish_connect()
      rescue error: WouldBlockError
        errors += 1
      end
      begin
        socket.read(1)
      rescue error: WouldBlockError
        errors += 1
      end
      begin
        socket.write("x")
      rescue error: WouldBlockError
        errors += 1
      end
      unless errors == 3 then raise "pending connect reported success" end
      puts("pending")
    elsif mode == "success" || mode == "raw"
      if mode == "raw"
        socket = TCPSocket.connect_nonblocking(address, port)
        loop do
          begin
            socket.finish_connect()
            break
          rescue error: WouldBlockError
            token.poll([], [socket])
          end
        end
      else
        socket = token.connect(address, port)
      end
      unless socket is Socket then raise "wrong socket kind" end
      socket.finish_connect()
      token.write(socket, "hello")
      response = ""
      while response.length() < 5
        response += token.read(socket, 5 - response.length())
      end
      unless response == "world" then raise "bad response" end
      unless token.read(socket, 1) == nil then raise "missing EOF" end
      puts("success")
    else
      if mode == "cancel" then worker = Thread.new(cancel_later, source) end
      puts("connecting")
      socket = token.connect(if mode == "pre" then "not-an-address" else address end, port)
      raise "cancelled connection returned"
    end
  rescue error: Cancellation::DeadlineExceeded
    unless mode == "deadline" then raise error end
    puts("deadline")
  rescue error: Cancellation::Cancelled
    unless mode == "pre" || mode == "cancel" || mode == "signal" then raise error end
    puts("cancelled")
  ensure
    if socket != nil then socket.close(); socket.close() end
    if worker != nil then worker.join() end
    puts("cleaned")
  end
end
run(ARGV[0], ARGV[1].to_i(), ARGV[2])
exit(0)
