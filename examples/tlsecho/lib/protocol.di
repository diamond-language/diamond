# The wire protocol. One request per line; the reply is one line, except
# GZIP, whose reply is a header line `GZ <n>` followed by exactly n bytes of
# gzip data. Framing binary data behind a length is what lets it share one
# connection with the text lines.
#
#   ECHO text     the text back
#   UPPER text    the text upper-cased
#   PROTO         the ALPN protocol this connection negotiated
#   WHOAMI        who the server sees: the client certificate's subject, or
#                 "anonymous" when the client presented none
#   GZIP text     (tlsecho/2 only) the text repeated 20 times, gzip-compressed
#   QUIT          BYE, then the server closes the connection
#
# A "reply" is a Hash: {"line": String, "body": String | nil, "close": Bool}.

# The most decompressed data the client will accept from a GZIP reply (1
# MiB), so a hostile server cannot make it inflate an enormous payload.
GUNZIP_LIMIT = 1_048_576

# Makes a reply Hash (see the header for its shape).
def build_reply(line: String, body: String | Nil = nil, close: Bool = false) -> Hash
  {"line": line, "body": body, "close": close}
end

# Turns one request line into a reply. Pure: no I/O, so it can be tested
# without a connection. `protocol` is the negotiated ALPN protocol and `peer`
# the client certificate's subject (nil without one).
def respond(request: String, protocol: String | Nil, peer: String | Nil = nil) -> Hash
  [command, text] = split_command(request)

  case command
  when "ECHO" then build_reply(text)
  when "UPPER" then build_reply(text.upcase())
  when "PROTO" then build_reply(if protocol == nil then "none" else protocol end)
  when "WHOAMI" then build_reply(if peer == nil then "anonymous" else peer end)
  # GZIP exists only in version 2 of the protocol, which is what makes ALPN
  # negotiation observable. The text is repeated 20 times so there is
  # something worth compressing. The header line carries the byte count of
  # the binary body that follows.
  when "GZIP"
    if protocol != "tlsecho/2"
      build_reply("ERR GZIP needs tlsecho/2")
    else
      packed = Gzip.compress(((text + "\n") * 20))
      build_reply("GZ #{packed.length()}", packed)
    end
  when "QUIT" then build_reply("BYE", nil, true)
  else build_reply("ERR unknown command '#{command}'")
  end
end

# "echo hi there" -> ["ECHO", "hi there"]. The command is case-insensitive;
# the text keeps its case and spacing. A line with no space is all command.
def split_command(request: String) -> Array
  at = request.index_of(" ")
  return [request.strip().upcase(), ""] if at == nil
  [request.slice(0, at).upcase(), request.slice(at + 1, request.length())]
end

# Reads exactly `count` bytes; a TLS read may return fewer than asked for.
def read_exactly(conn, count: Int) -> String
  data = ""
  while data.length() < count
    chunk = conn.read(count - data.length())
    raise IOError.new("connection closed after #{data.length()} of #{count} bytes") if chunk == nil
    data = data + chunk
  end
  data
end
