require "../lib/cancellation"

def cancel_on_marker(source, marker)
  wait = Cancellation::Source.new().token()
  until File.exist?(marker)
    wait.sleep(0.001)
  end
  source.cancel()
end

def parallel_lookup(gate)
  gate.receive()
  begin
    Cancellation::Source.new(nil, 1.0).token().resolve("slow.test")
    raise "stalled lookup returned"
  rescue error: Cancellation::DeadlineExceeded
    "deadline"
  rescue error: IOError
    unless error.message().include?("capacity exhausted") then raise error end
    "capacity"
  end
end

def run(mode, port, marker)
  source = Cancellation::Source.new(nil, if mode == "deadline" then 0.2 elsif mode == "budget" then 1.0 else nil end)
  token = Cancellation::Source.new(source.token()).token()
  worker = nil
  def cancel() = source.cancel()
  def fail_signal()
    raise RuntimeError.new("signal handler failed")
  end
  if mode == "signal" then Signal.trap("TERM", cancel) end
  if mode == "signal_error" then Signal.trap("TERM", fail_signal) end
  begin
    if mode == "success"
      unless token.resolve("multi.test") == ["::1", "127.0.0.1"] then raise "resolver order/deduplication" end
      socket = token.connect("multi.test", port)
      begin
        token.write(socket, "hello")
      ensure
        socket.close()
      end
      puts("success")
    elsif mode == "missing"
      begin
        token.resolve("missing.test")
        raise "missing host resolved"
      rescue error: IOError
        puts("missing")
      end
    elsif mode == "parallel"
      gate = Channel.new(16)
      workers = []
      16.times() do |i| workers.push(Thread.new(parallel_lookup, gate)) end
      16.times() do |i| gate.send(true) end
      deadlines = 0
      capacities = 0
      workers.each() do |thread|
        result = thread.join()
        if result == "deadline" then deadlines += 1 else capacities += 1 end
      end
      unless deadlines == 8 && capacities == 8 then raise "resolver concurrency cap failed" end
      puts("parallel bounded")
    elsif mode == "capacity"
      puts("baseline")
      gets()
      8.times() do |i|
        begin
          Cancellation::Source.new(nil, 0.04).token().resolve("slow.test")
          raise "stalled DNS returned"
        rescue error: Cancellation::DeadlineExceeded
          nil
        end
      end
      begin
        token.resolve("slow.test")
        raise "resolver capacity was unbounded"
      rescue error: IOError
        unless error.message().include?("capacity exhausted") then raise error end
      end
      unless token.resolve("127.0.0.1") == ["127.0.0.1"] then raise "numeric lookup needs a worker" end
      puts("saturated")
      gets()
      # Released workers eventually return their slots and descriptors.
      loop do
        begin
          token.resolve("multi.test")
          break
        rescue error: IOError
          token.sleep(0.01)
        end
      end
      token.sleep(0.1)
      puts("recovered")
      gets()
    else
      if mode == "cancel" then worker = Thread.new(cancel_on_marker, source, marker) end
      started = Time.monotonic()
      if mode == "budget"
        token.connect("budget.test", port)
      else
        token.resolve("slow.test")
      end
      raise "stalled lookup returned"
    end
  rescue error: RuntimeError
    if mode != "signal_error" || error.message() != "signal handler failed" then raise error end
    puts("signal error")
  rescue error: Cancellation::DeadlineExceeded
    if mode != "deadline" && mode != "budget" then raise error end
    if mode == "budget" && Time.monotonic() - started >= 1.4 then raise "connection restarted deadline" end
    puts("deadline")
  rescue error: Cancellation::Cancelled
    if mode != "cancel" && mode != "signal" then raise error end
    puts("cancelled")
  ensure
    if worker != nil then worker.join() end
    puts("cleaned")
  end
end
run(ARGV[0], ARGV[1].to_i(), ARGV[2])
exit(0)
