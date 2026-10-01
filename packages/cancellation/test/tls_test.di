require "../lib/cancellation"

def cancel_later(source)
  Cancellation::Source.new().token().sleep(0.15)
  source.cancel()
end

def run(port, ca, mode)
  timeout = if mode == "deadline" then 0.3 elsif mode == "budget" then 1.0 else 5.0 end
  source = Cancellation::Source.new(nil, timeout)
  token = Cancellation::Source.new(source.token()).token()
  socket = nil
  worker = nil
  def cancel() = source.cancel()
  def signal_error()
    raise RuntimeError.new("signal handler failed")
  end
  if mode == "signal" then Signal.trap("TERM", cancel) end
  if mode == "signal_error" then Signal.trap("TERM", signal_error) end
  if mode == "cancel" then worker = Thread.new(cancel_later, source) end
  if mode == "pre" then source.cancel() end
  options = {"ca_file": ca, "alpn": ["diamond-test"], "read_timeout_ms": 2000, "write_timeout_ms": 2000}
  if mode == "untrusted" then options = nil end
  if mode == "setup_error" then options = {"ca_file": "/no/such/diamond-ca.pem"} end
  host = if mode == "ip" then "127.0.0.1" elsif mode == "budget" then "budget.test" else "localhost" end
  started = Time.monotonic()
  begin
    if mode == "leaks"
      puts("baseline")
      gets()
      20.times() do |i|
        begin
          Cancellation::Source.new(nil, 0.04).token().connect_tls(host, port, options)
          raise "stalled TLS handshake succeeded"
        rescue error: Cancellation::DeadlineExceeded
          nil
        end
      end
      puts("no leaks")
      gets()
      return nil
    end
    if mode == "raw" || mode == "pending" || mode == "validation"
      tcp = token.connect(host, port)
      if mode == "validation"
        begin
          TLSSocket.start_handshake(tcp, host, {"connect_timeout_ms": 100})
          raise "unsupported timeout accepted"
        rescue error: TypeError
          nil
        end
      end
      socket = TLSSocket.start_handshake(tcp, host, options)
      begin
        tcp.write("plaintext")
        raise "TCP ownership was not transferred"
      rescue error: IOError
        nil
      end
      if mode == "pending"
        begin
          socket.write("plaintext")
          raise "TLS application write accepted during handshake"
        rescue error: IOError
          nil
        end
        direction = socket.finish_handshake()
        unless direction == "read" || direction == "write" then raise "expected pending handshake" end
        socket.abort()
        socket.abort()
        begin
          socket.finish_handshake()
          raise "aborted TLS socket resurrected"
        rescue error: IOError
          nil
        end
        puts("pending")
        return nil
      end
      loop do
        direction = socket.finish_handshake()
        if direction == nil then break end
        token.poll(if direction == "read" then [socket] else [] end, if direction == "write" then [socket] else [] end)
      end
      unless socket.finish_handshake() == nil then raise "completion not idempotent" end
    else
      socket = token.connect_tls(host, port, options)
    end
    unless mode == "success" || mode == "ip" || mode == "raw" || mode == "validation" then raise "unexpected handshake success" end
    unless socket.alpn_protocol() == "diamond-test" then raise "ALPN missing" end
    socket.write("hello")
    unless socket.read(5) == "world" then raise "TLS response mismatch" end
    puts("success")
  rescue error: Cancellation::DeadlineExceeded
    unless mode == "deadline" || mode == "budget" then raise error end
    if mode == "budget" && Time.monotonic() - started > 1.45 then raise "deadline restarted" end
    puts("deadline")
  rescue error: Cancellation::Cancelled
    unless mode == "cancel" || mode == "signal" || mode == "pre" then raise error end
    puts("cancelled")
  rescue error: IOError
    unless mode == "untrusted" || mode == "wronghost" || mode == "setup_error" then raise error end
    puts("certificate rejected")
  rescue error: RuntimeError
    unless mode == "signal_error" && error.message() == "signal handler failed" then raise error end
    puts("signal error")
  ensure
    if socket != nil then socket.close() end
    if worker != nil then worker.join() end
    puts("cleaned")
  end
end
run(ARGV[0].to_i(), ARGV[1], ARGV[2])
exit(0)
