# tlsecho: a small line service over TLS, and a client that verifies it.
#
# tlsecho.di serve PORT CERT KEY [--client-ca CAFILE]
# tlsecho.di client HOST PORT --ca CAFILE [--cert FILE --key FILE] [--alpn LIST]
#                  [--resume] COMMAND...
#
# The server offers ALPN protocols tlsecho/2 then tlsecho/1 and answers the
# commands in lib/protocol.di. With --client-ca it also demands a client
# certificate signed by that CA (mutual TLS) and refuses anyone without one.
# The client always verifies the server's certificate against CAFILE and the
# HOST name (there is no way to turn that off), optionally presents its own
# certificate with --cert/--key, sends each COMMAND as a line, and prints
# each reply. --alpn takes a comma-separated preference list; --resume
# reconnects with the first connection's session and reports whether TLS
# resumed it.
require "./lib/protocol"

def usage() -> Int
  warn("usage: tlsecho.di serve PORT CERT KEY [--client-ca CAFILE]")
  warn("       tlsecho.di client HOST PORT --ca CAFILE [--cert FILE --key FILE] [--alpn LIST] [--resume] COMMAND...")
  64
end

# A port from the command line; anything to_i() would quietly forgive ("80x")
# is rejected by the round-trip check.
def parse_port(text: String) -> Int
  port = text.to_i()
  raise ArgumentError.new("bad port '#{text}'") unless port.to_s() == text && port >= 1 && port <= 65535
  port
end

# Serves one connection until QUIT or the peer goes away. Returns the number
# of requests handled.
def serve_connection(conn) -> Int
  # Facts about this connection, fixed after the handshake: the negotiated
  # protocol and (with mutual TLS) who the client is.
  protocol = conn.alpn_protocol()
  peer = conn.peer_subject()
  handled = 0

  loop do
    # gets() returns nil when the client hangs up.
    request = conn.gets()
    break if request == nil

    # Write the reply line, then the binary body (only GZIP has one).
    reply = respond(request, protocol, peer)
    conn.write(reply["line"] + "\n")
    conn.write(reply["body"]) unless reply["body"] == nil
    handled += 1
    break if reply["close"]
  end
  handled
end

# The server: accept connections one at a time, forever.
def serve(port: Int, cert: String, key: String, client_ca: String | Nil) -> Int
  # Offer protocol 2 ahead of protocol 1 (listed in preference order). Naming a
  # client CA turns on mutual TLS: clients must present a certificate it
  # signed.
  options = {"alpn": ["tlsecho/2", "tlsecho/1"]}
  options["client_ca"] = client_ca unless client_ca == nil
  listener = TLSServer.listen(port, cert, key, options)
  served = 0

  # accept() is interruptible: a trapped signal runs this, which ends the run.
  def on_signal()
    puts("stopping after #{served} connections")
    exit(0)
  end
  Signal.trap("TERM", on_signal)
  Signal.trap("INT", on_signal)

  puts("listening on tls/#{port}")

  # The accept loop. accept() performs the TLS handshake, so a failed
  # handshake surfaces here, and it must not stop the server.
  loop do
    begin
      conn = listener.accept()
    rescue error: IOError
      # A client that rejects our certificate aborts the handshake, and with
      # --client-ca so does a client with no acceptable certificate of its
      # own; neither is a reason for the server to stop.
      puts("handshake failed")
      next
    end

    # Handle the session. A connection that dies mid-conversation is logged
    # and dropped; either way the connection is closed afterwards.
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

# Sends one command and prints a one-line summary of the reply.
def print_reply(conn, command: String)
  conn.write(command + "\n")
  head = conn.gets()
  raise IOError.new("server closed the connection") if head == nil

  # A "GZ <n>" header means n bytes of gzip data follow: read exactly that
  # many (read_exactly, since one TLS read can return fewer), decompress
  # with the safety limit, and report what arrived.
  if head.start_with?("GZ ")
    size = head.slice(3, head.length()).to_i()
    text = Gzip.decompress(read_exactly(conn, size), GUNZIP_LIMIT)
    lines = text.strip().split("\n")
    puts("#{command} -> #{lines.length()} lines of '#{lines[0]}', #{text.length()} bytes, compressed on the wire: #{size < text.length()}")
  else
    puts("#{command} -> #{head}")
  end
end

# Connects over TLS. The CA file is always supplied, so the server's
# certificate is always verified (against that CA and the host name). The
# other options are added only when given: a client certificate, an ALPN
# preference list, and a saved session to resume.
def open_connection(host: String, port: Int, ca: String, identity: Array | Nil,
                    alpn: Array | Nil, session: String | Nil)
  options = {"ca_file": ca}
  unless identity == nil
    options["cert"] = identity[0]
    options["key"] = identity[1]
  end
  options["alpn"] = alpn unless alpn == nil
  options["session"] = session unless session == nil
  TLSSocket.connect(host, port, options)
end

def run_client(host: String, port: Int, ca: String, identity: Array | Nil, alpn: Array | Nil,
               resume: Bool, commands: Array) -> Int
  # First connection: report the negotiated protocol and run the commands.
  conn = open_connection(host, port, ca, identity, alpn, nil)
  puts("negotiated: #{conn.alpn_protocol() == nil ? "none" : conn.alpn_protocol()}")
  commands.each() do |command| print_reply(conn, command) end

  # Save the session ticket BEFORE closing, then say goodbye politely (QUIT
  # makes the server end the conversation cleanly).
  session = conn.session()
  conn.write("QUIT\n")
  conn.gets()
  conn.close()

  # --resume: connect again presenting the saved session. `session_reused?`
  # says whether TLS skipped the full handshake. One cheap request is needed
  # so the handshake actually completes before asking.
  if resume
    if session == nil
      puts("resumption: no session to resume")
      return 1
    end
    again = open_connection(host, port, ca, identity, alpn, session)
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

  # Two subcommands. A bad port is a usage error (64); a connection or TLS
  # failure is 66.
  begin
    case argv[0]
    when "serve"
      return usage() unless argv.length() == 4 || (argv.length() == 6 && argv[4] == "--client-ca")
      serve(parse_port(argv[1]), argv[2], argv[3], argv.length() == 6 ? argv[5] : nil)
    when "client"
      return usage() if argv.length() < 5

      # Positional HOST and PORT, then options and commands in any order.
      host = argv[1]
      port = parse_port(argv[2])
      ca = nil
      cert = nil
      key = nil
      alpn = nil
      resume = false
      commands = []
      index = 3

      # Every option takes a value except --resume; anything not an option is
      # one of the commands to send.
      while index < argv.length()
        case argv[index]
        when "--ca"
          index += 1
          return usage() if index >= argv.length()
          ca = argv[index]
        when "--cert"
          index += 1
          return usage() if index >= argv.length()
          cert = argv[index]
        when "--key"
          index += 1
          return usage() if index >= argv.length()
          key = argv[index]
        when "--alpn"
          index += 1
          return usage() if index >= argv.length()
          alpn = argv[index].split(",")
        when "--resume" then resume = true
        else commands.push(argv[index])
        end
        index += 1
      end

      # Required: a CA and at least one command. --cert and --key must come
      # as a pair (exactly one given is an error).
      return usage() if ca == nil || commands.empty?() || (cert == nil) != (key == nil)
      identity = if cert == nil then nil else [cert, key] end
      run_client(host, port, ca, identity, alpn, resume, commands)
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
