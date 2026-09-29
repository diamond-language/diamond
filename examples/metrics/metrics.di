# metrics: a StatsD-style metrics collector over UDP.
#
# metrics.di serve PORT             collect datagrams until SIGINT/SIGTERM or !stop
# metrics.di send PORT LINE...      send each LINE as one datagram, print each reply
# metrics.di report PORT [--json]   ask a running server for its aggregates
# metrics.di aggregate FILE         aggregate a file of metric lines offline
#
# Metric lines look like `requests:1|c`, `queue_depth:7|g`, `latency:12.5|ms`
# (see lib/aggregate.di). A datagram is answered with `ok N` or `error: ...`.
# Control datagrams: !ping, !report, !text, !reset, !stop.
require "./lib/aggregate"

def usage() -> Int
  warn("usage: metrics.di serve PORT | send PORT LINE... | report PORT [--json] | aggregate FILE")
  64
end

def parse_port(text: String) -> Int
  port = text.to_i()
  raise ArgumentError.new("bad port '#{text}'") unless port.to_s() == text && port >= 1 && port <= 65535
  port
end

# The reply to one datagram, and whether the server should stop after it.
def answer(aggregator: Aggregator, datagram: String) -> Array
  case datagram.strip()
  when "!ping" then ["pong", false]
  when "!report" then [JSON.stringify(aggregator.report()), false]
  when "!text" then [aggregator.to_text(), false]
  when "!reset"
    aggregator.reset()
    ["reset", false]
  when "!stop" then ["stopping", true]
  else
    begin
      ["ok #{aggregator.record_lines(datagram)}", false]
    rescue error: MetricError
      ["error: #{error.message()}", false]
    end
  end
end

def serve(port: Int) -> Int
  aggregator = Aggregator.new()
  socket = UDPSocket.bind(port)

  # The receive below blocks with nothing to do; a signal interrupts it, runs
  # this handler, and the handler ends the program: print what was collected,
  # then exit. A trapped handler must be a nested def (a closure value).
  def on_signal()
    puts("")
    puts("signal received; final totals:")
    puts(aggregator.to_text())
    exit(0)
  end
  Signal.trap("INT", on_signal)
  Signal.trap("TERM", on_signal)

  puts("listening on udp/#{port}")
  stopping = false
  until stopping
    packet = socket.receive(4096)
    [reply, stopping] = answer(aggregator, packet["data"])
    socket.send(reply, packet["host"], packet["port"])
  end
  socket.close()
  puts("stopped by !stop; final totals:")
  puts(aggregator.to_text())
  0
end

# Each line is its own datagram, and each is answered before the next is
# sent. UDP has no delivery guarantee and IO.poll doesn't take UDP sockets,
# so there is no timeout: with no server listening this would wait forever.
def send_lines(port: Int, lines: Array) -> Int
  socket = UDPSocket.open()
  failed = 0
  lines.each() do |line|
    socket.send(line, "127.0.0.1", port)
    reply = socket.receive(4096)["data"]
    puts(reply)
    failed += 1 if reply.start_with?("error")
  end
  socket.close()
  failed == 0 ? 0 : 1
end

def fetch_report(port: Int, json: Bool) -> Int
  socket = UDPSocket.open()
  socket.send(json ? "!report" : "!text", "127.0.0.1", port)
  puts(socket.receive(65535)["data"])
  socket.close()
  0
end

def aggregate_file(path: String) -> Int
  aggregator = Aggregator.new()
  File.read(path).split("\n").each_with_index() do |line, index|
    text = line.strip()
    next if text.empty?() || text.start_with?("#")
    begin
      aggregator.record_lines(text)
    rescue error: MetricError
      warn("#{path}:#{index + 1}: #{error.message()}")
      return 65
    end
  end
  puts(aggregator.to_text())
  0
end

def main(argv) -> Int
  return usage() if argv.empty?()
  begin
    case argv[0]
    when "serve"
      return usage() unless argv.length() == 2
      serve(parse_port(argv[1]))
    when "send"
      return usage() if argv.length() < 3
      send_lines(parse_port(argv[1]), argv.slice(2, argv.length()))
    when "report"
      return usage() unless argv.length() == 2 || (argv.length() == 3 && argv[2] == "--json")
      fetch_report(parse_port(argv[1]), argv.length() == 3)
    when "aggregate"
      return usage() unless argv.length() == 2
      aggregate_file(argv[1])
    else
      usage()
    end
  rescue error: ArgumentError
    warn(error.message())
    64
  rescue error: IOError
    warn(error.message())
    66
  end
end

exit(main(ARGV))
