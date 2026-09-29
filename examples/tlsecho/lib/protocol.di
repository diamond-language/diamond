# The wire protocol. One request per line; the reply is one line, except
# GZIP, whose reply is a header line `GZ <n>` followed by exactly n bytes of
# gzip data. Framing binary data behind a length is what lets it share one
# connection with the text lines.
#
#   ECHO text     the text back
#   UPPER text    the text upper-cased
#   PROTO         the ALPN protocol this connection negotiated
#   GZIP text     (tlsecho/2 only) the text repeated 20 times, gzip-compressed
#   QUIT          BYE, then the server closes the connection
#
# A "reply" is a Hash: {"line": String, "body": String | nil, "close": Bool}.

def gunzip_limit() -> Int = 1_048_576

def build_reply(line: String, body: String | Nil = nil, close: Bool = false) -> Hash
  {"line": line, "body": body, "close": close}
end

def respond(request: String, protocol: String | Nil) -> Hash
  [command, text] = split_command(request)
  case command
  when "ECHO" then build_reply(text)
  when "UPPER" then build_reply(text.upcase())
  when "PROTO" then build_reply(if protocol == nil then "none" else protocol end)
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
