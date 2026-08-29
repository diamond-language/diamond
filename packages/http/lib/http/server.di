def http_parse_request(conn)
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
