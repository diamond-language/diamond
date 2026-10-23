class HttpRequestError < StandardError
end

# Bounded parsing is opt-in; Gremlin supplies per-service limits.
def http_bounded_line(conn, maximum)
  line = ""
  loop do
    byte = conn.read(1)
    if byte == "" then raise HttpRequestError.new("400") end
    line = line + byte
    if line.length() > maximum then raise HttpRequestError.new("431") end
    if byte == "\n"
      if line.length() < 2 || line.slice(line.length() - 2, 2) != "\r\n" then raise HttpRequestError.new("400") end
      return line.slice(0, line.length() - 2)
    end
  end
end

def http_parse_bounded_request(conn, limits)
  line = http_bounded_line(conn, limits["line_bytes"])
  pieces = line.split(" ")
  if pieces.length() != 3 || !["HTTP/1.1", "HTTP/1.0"].include?(pieces[2]) || !pieces[1].start_with?("/")
    raise HttpRequestError.new("400")
  end
  unless ["GET", "POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS"].include?(pieces[0]) then raise HttpRequestError.new("400") end
  pieces[1].chars().each() do |ch|
    code = ch.ord()
    if code <= 32 || code >= 127 then raise HttpRequestError.new("400") end
  end
  headers = {}
  total = line.length() + 2
  count = 0
  loop do
    line = http_bounded_line(conn, limits["line_bytes"])
    total += line.length() + 2
    if total > limits["header_bytes"] then raise HttpRequestError.new("431") end
    if line == "" then break end
    count += 1
    if count > limits["header_count"] then raise HttpRequestError.new("431") end
    colon = line.index_of(":")
    if colon == nil || colon == 0 then raise HttpRequestError.new("400") end
    name = line.slice(0, colon).downcase()
    name.chars().each() do |ch|
      code = ch.ord()
      if !((code >= 97 && code <= 122) || (code >= 48 && code <= 57) || "!#$%&'*+-.^_`|~".include?(ch))
        raise HttpRequestError.new("400")
      end
    end
    if headers[name] != nil then raise HttpRequestError.new("400") end
    value = line.slice(colon + 1, line.length() - colon - 1).strip()
    value.chars().each() do |ch|
      code = ch.ord()
      if (code < 32 && code != 9) || code == 127 then raise HttpRequestError.new("400") end
    end
    headers[name] = value
  end
  if headers["transfer-encoding"] != nil then raise HttpRequestError.new("400") end
  if headers["expect"] != nil && headers["expect"].downcase() != "100-continue" then raise HttpRequestError.new("417") end
  if pieces[2] == "HTTP/1.1" && (headers["host"] == nil || headers["host"] == "") then raise HttpRequestError.new("400") end
  length = 0
  if headers["content-length"] != nil
    text = headers["content-length"]
    length = text.to_i()
    if "#{length}" != text || length < 0 then raise HttpRequestError.new("400") end
    if length > limits["body_bytes"] then raise HttpRequestError.new("413") end
  end
  if headers["expect"] != nil then conn.write("HTTP/1.1 100 Continue\r\n\r\n") end
  body = conn.read(length)
  if body.length() != length then raise HttpRequestError.new("400") end
  {"method": pieces[0], "path": pieces[1], "headers": headers, "body": body}
end

def http_parse_request(conn, limits = nil)
  if limits != nil then return http_parse_bounded_request(conn, limits) end
  request_line = conn.gets()
  if request_line == nil
    return nil
  end
  method_end = request_line.index_of(" ")
  method = request_line.slice(0, method_end)
  after_method = request_line.slice(method_end + 1, request_line.length())
  path_end = after_method.index_of(" ")
  path = after_method.slice(0, path_end)

  headers = {}
  def collect_header(line)
    colon = line.index_of(": ")
    unless colon == nil
      name = line.slice(0, colon)
      value = line.slice(colon + 2, line.length())
      headers[name.downcase()] = value
    end
  end
  loop do
    line = conn.gets()
    if line == nil || line == ""
      break
    end
    collect_header(line)
  end

  body = ""
  content_length = headers["content-length"]
  unless content_length == nil
    length = content_length.to_i()
    # Treated like the "connection closed before sending a request
    # line" case above (return nil, connection closed, server moves on
    # to the next accept()) rather than raised -- http_serve's own
    # accept loop has no rescue around this call, so raising here would
    # crash the whole server on a single oversized request instead of
    # just refusing this one connection.
    if length > http_max_body_size()
      return nil
    end
    body = conn.read(length)
  end

  {"method": method, "path": path, "headers": headers, "body": body}
end

def http_write_response(conn, response)
  [status, headers, body] = response
  conn.write("HTTP/1.1 #{status} #{http_status_text(status)}\r\n")
  # A header value is ordinarily a single String, written as one line --
  # but some headers (Set-Cookie chief among them) legitimately need
  # several distinct lines with the same name in one response, and
  # per-HTTP-spec Set-Cookie's own value can contain a comma (its
  # Expires=... attribute), so the usual "join repeated values with a
  # comma" trick other headers use doesn't apply here. An Array value
  # writes one line per element instead of one line for the whole thing;
  # every existing caller only ever puts a String in `headers`, so this
  # is purely additive.
  def write_header(name, value)
    case value
    when [*lines]
      def write_line(line)
        conn.write("#{name}: #{line}\r\n")
      end
      lines.each(write_line)
    else
      conn.write("#{name}: #{value}\r\n")
    end
  end
  headers.each(write_header)
  conn.write("Content-Length: #{body.length()}\r\n")
  conn.write("\r\n")
  conn.write(body)
end

def http_serve(port, handler: Callable[1])
  server = TCPServer.listen(port)
  loop do
    conn = server.accept()
    request = http_parse_request(conn)
    unless request == nil
      response = handler(request)
      http_write_response(conn, response)
    end
    conn.close()
  end
end
