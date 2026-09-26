# kvserver: a small networked key-value store.
#
#   diamond kvserver.di PORT LOGFILE
#
# One thread, one IO.poll loop over the listening socket and every client.
# Clients may send requests in pieces or several per packet; each
# connection buffers input until a full line arrives and buffers replies
# until the socket can take them. SIGINT/SIGTERM stop the loop cleanly:
# pending replies are sent, the log is compacted and closed.
require "./lib/protocol"

class Connection
  attr_reader socket
  attr_predicate closing: Bool

  def initialize(socket)
    @socket = socket
    @input = ""
    @output = ""
    @closing = false
  end

  def wants_write?() -> Bool = !@output.empty?()

  # Reads what's available and answers every complete line. False once the
  # peer has hung up.
  def receive(store: Store) -> Bool
    loop do
      chunk = nil
      begin
        chunk = @socket.read(4096)
      rescue error: WouldBlockError
        break
      end
      return false if chunk == nil
      @input = @input + chunk
    end
    loop do
      newline = @input.index_of("\n")
      break if newline == nil
      line = @input.slice(0, newline).rstrip()
      @input = @input.slice(newline + 1, @input.length())
      reply = handle(store, line, Time.utc_now().to_f())
      if reply == nil
        @output = @output + "+BYE\n"
        @closing = true
        break
      end
      @output = @output + reply + "\n"
    end
    true
  end

  # Writes as much pending output as the socket takes (writes can be
  # partial). True once nothing is left.
  def send_pending() -> Bool
    while !@output.empty?()
      written = @socket.write(@output)
      break if written == 0
      @output = @output.slice(written, @output.length())
    end
    @output.empty?()
  end

  def close()
    @socket.close()
  end
end

def main(args: Array[String]) -> Int
  if args.length() != 2 || args[0].to_i() < 1
    warn("usage: kvserver PORT LOGFILE")
    return 64
  end
  port = args[0].to_i()
  store = Store.new(args[1])
  loaded = store.load()
  listener = TCPServer.listen_nonblocking(port)
  puts("kvserver: #{loaded} key(s) loaded, listening on port #{port}")

  stopping = false
  def stop()
    stopping = true
  end
  Signal.trap("INT", stop)
  Signal.trap("TERM", stop)

  connections = []
  last_maintenance = Time.monotonic()
  until stopping
    readables = [listener, *connections.map() do |connection| connection.socket() end]
    writers = connections.select() do |connection| connection.wants_write?() end
    ready = IO.poll(readables, writers.map() do |connection| connection.socket() end, 500)
    break if stopping

    # ready[...] lines up with the connections that were polled, so new
    # ones join the list only after those have been handled.
    accepted = []
    if ready["readable"][0]
      loop do
        socket = listener.accept()
        break if socket == nil
        accepted.push(Connection.new(socket))
      end
    end
    kept = []
    connections.each_with_index() do |connection, index|
      hung_up = ready["readable"][index + 1] && !connection.receive(store)
      connection.send_pending()
      # Close when the peer left, or after QUIT once its reply is out.
      if hung_up || (connection.closing?() && !connection.wants_write?())
        connection.close()
      else
        kept.push(connection)
      end
    end
    connections = kept.concat(accepted)

    # Expire keys and compact the log about once a second.
    if Time.monotonic() - last_maintenance >= 1.0
      now = Time.utc_now().to_f()
      store.sweep(now)
      store.compact_if_bloated(now)
      last_maintenance = Time.monotonic()
    end
  end

  connections.each() do |connection|
    connection.send_pending()
    connection.close()
  end
  listener.close()
  store.compact_if_bloated(Time.utc_now().to_f())
  size = store.size(Time.utc_now().to_f())
  store.close()
  puts("kvserver: stopped with #{size} key(s)")
  0
end

exit(main(ARGV))
