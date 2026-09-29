# tlsecho: a small line service over TLS, and a client that verifies it.
#
# tlsecho.di serve PORT CERT KEY
# tlsecho.di client HOST PORT --ca CAFILE [--alpn LIST] [--resume] COMMAND...
#
# The server offers ALPN protocols tlsecho/2 then tlsecho/1 and answers the
# commands in lib/protocol.di. The client always verifies the server's
# certificate against CAFILE and the HOST name (there is no way to turn that
# off), sends each COMMAND as a line, and prints each reply. --alpn takes a
# comma-separated preference list; --resume reconnects with the first
# connection's session and reports whether TLS resumed it.
require "./lib/protocol"

def usage() -> Int
  warn("usage: tlsecho.di serve PORT CERT KEY")
  warn("       tlsecho.di client HOST PORT --ca CAFILE [--alpn LIST] [--resume] COMMAND...")
  64
end

def parse_port(text: String) -> Int
  port = text.to_i()
  raise ArgumentError.new("bad port '#{text}'") unless port.to_s() == text && port >= 1 && port <= 65535
  port
end

# Serves one connection until QUIT or the peer goes away. Returns the number
# of requests handled.
def serve_connection(conn) -> Int
  protocol = conn.alpn_protocol()
  handled = 0
  loop do
    request = conn.gets()
    break if request == nil
    reply = respond(request, protocol)
    conn.write(reply["line"] + "\n")
    conn.write(reply["body"]) unless reply["body"] == nil
    handled += 1
    break if reply["close"]
  end
  handled
end

def serve(port: Int, cert: String, key: String) -> Int
  listener = TLSServer.listen(port, cert, key, {"alpn": ["tlsecho/2", "tlsecho/1"]})
  served = 0

  # accept() is interruptible: a trapped signal runs this, which ends the run.
  def on_signal()
    puts("stopping after #{served} connections")
    exit(0)
  end
  Signal.trap("TERM", on_signal)
  Signal.trap("INT", on_signal)

  puts("listening on tls/#{port}")
  loop do
    begin
      conn = listener.accept()
    rescue error: IOError
      # A client that rejects our certificate aborts the handshake; that is
      # that client's decision and no reason for the server to stop.
      puts("handshake failed")
      next
    end
    served += 1
    begin
      count = serve_connection(conn)
      puts("connection #{served}: #{conn.alpn_protocol() == nil ? "no protocol" : conn.alpn_protocol()}, #{count} requests")
    rescue error: IOError
      puts("connection #{served}: dropped")
    end
    conn.close()
  end
  0
end

def print_reply(conn, command: String)
  conn.write(command + "\n")
  head = conn.gets()
  raise IOError.new("server closed the connection") if head == nil
  if head.start_with?("GZ ")
    size = head.slice(3, head.length()).to_i()
    text = Gzip.decompress(read_exactly(conn, size), gunzip_limit())
    lines = text.strip().split("\n")
    puts("#{command} -> #{lines.length()} lines of '#{lines[0]}', #{text.length()} bytes, compressed on the wire: #{size < text.length()}")
  else
    puts("#{command} -> #{head}")
  end
end

def open_connection(host: String, port: Int, ca: String, alpn: Array | Nil, session: String | Nil)
  options = {"ca_file": ca}
  options["alpn"] = alpn unless alpn == nil
  options["session"] = session unless session == nil
  TLSSocket.connect(host, port, options)
end

def run_client(host: String, port: Int, ca: String, alpn: Array | Nil, resume: Bool, commands: Array) -> Int
  conn = open_connection(host, port, ca, alpn, nil)
  puts("negotiated: #{conn.alpn_protocol() == nil ? "none" : conn.alpn_protocol()}")
  commands.each() do |command| print_reply(conn, command) end
  session = conn.session()
  conn.write("QUIT\n")
  conn.gets()
  conn.close()
  if resume
    if session == nil
      puts("resumption: no session to resume")
      return 1
    end
    again = open_connection(host, port, ca, alpn, session)
    again.write("PROTO\n")
    again.gets()
    puts("resumed: #{again.session_reused?()}")
    again.write("QUIT\n")
    again.gets()
    again.close()
  end
  0
end

def main(argv) -> Int
  return usage() if argv.empty?()
  begin
    case argv[0]
    when "serve"
      return usage() unless argv.length() == 4
      serve(parse_port(argv[1]), argv[2], argv[3])
    when "client"
      return usage() if argv.length() < 5
      host = argv[1]
      port = parse_port(argv[2])
      ca = nil
      alpn = nil
      resume = false
      commands = []
      index = 3
      while index < argv.length()
        case argv[index]
        when "--ca"
          index += 1
          return usage() if index >= argv.length()
          ca = argv[index]
        when "--alpn"
          index += 1
          return usage() if index >= argv.length()
          alpn = argv[index].split(",")
        when "--resume" then resume = true
        else commands.push(argv[index])
        end
        index += 1
      end
      return usage() if ca == nil || commands.empty?()
      run_client(host, port, ca, alpn, resume, commands)
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
