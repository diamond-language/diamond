# Client: http_get/http_post/http_request make a real outbound request
# to any host TCPSocket.connect can reach -- unlike http_serve above,
# not limited to loopback/localhost in any way. Plain http:// only,
# same "no TLS" stance TCPSocket itself has; https:// is rejected with
# a clear error rather than silently connecting in the clear on port
# 443. No redirect-following (a 3xx response comes back as-is, Location
# header and all -- the caller chases it manually if it wants to), no
# chunked transfer encoding (same scope cut http_serve already makes),
# no keep-alive (every request sends "Connection: close" and reads to
# EOF/Content-Length, then closes) -- deliberately as basic as
# http_serve's own server half.

def http_parse_url(url)
  scheme_end = url.index_of("://")
  if scheme_end == nil
    raise ArgumentError.new("invalid URL (missing scheme): #{url}")
  end
  scheme = url.slice(0, scheme_end)
  if scheme != "http"
    raise ArgumentError.new("unsupported URL scheme '#{scheme}' (only http is supported)")
  end
  rest = url.slice(scheme_end + 3, url.length())
  path_start = rest.index_of("/")
  host_and_port = rest
  path = "/"
  unless path_start == nil
    host_and_port = rest.slice(0, path_start)
    path = rest.slice(path_start, rest.length())
  end
  colon = host_and_port.index_of(":")
  host = host_and_port
  port = 80
  unless colon == nil
    host = host_and_port.slice(0, colon)
    port = host_and_port.slice(colon + 1, host_and_port.length()).to_i()
  end
  {"host": host, "port": port, "path": path}
end

def http_request(method, url, headers = {}, body = "")
  parsed = http_parse_url(url)
  conn = TCPSocket.connect(parsed["host"], parsed["port"])

  conn.write("#{method} #{parsed["path"]} HTTP/1.1\r\n")
  conn.write("Host: #{parsed["host"]}\r\n")
  def write_request_header(name, value)
    conn.write("#{name}: #{value}\r\n")
  end
  headers.each(write_request_header)
  if body.length() > 0
    conn.write("Content-Length: #{body.length()}\r\n")
  end
  conn.write("Connection: close\r\n")
  conn.write("\r\n")
  if body.length() > 0
    conn.write(body)
  end

  status_line = conn.gets()
  after_method = status_line.slice(status_line.index_of(" ") + 1, status_line.length())
  status = after_method.slice(0, after_method.index_of(" ")).to_i()

  response_headers = {}
  def collect_response_header(line)
    colon = line.index_of(": ")
    unless colon == nil
      name = line.slice(0, colon)
      value = line.slice(colon + 2, line.length())
      response_headers[name.downcase()] = value
    end
  end
  loop do
    line = conn.gets()
    if line == nil || line == ""
      break
    end
    collect_response_header(line)
  end

  response_body = ""
  content_length = response_headers["content-length"]
  if content_length != nil
    length = content_length.to_i()
    # Unlike the server side (http_parse_request), raising here is the
    # right behavior: this is a single outbound call, there's no next
    # connection to move on to, and an oversized response is exactly
    # the kind of malformed-response condition this package's own
    # policy (see the file-level comment above) already propagates as
    # an error rather than silently working around.
    if length > http_max_body_size()
      conn.close()
      raise IOError.new("response Content-Length #{length} exceeds maximum of #{http_max_body_size()}")
    end
    response_body = conn.read(length)
  else
    response_body = conn.read()
  end
  conn.close()

  {"status": status, "headers": response_headers, "body": response_body}
end

def http_get(url, headers = {})
  http_request("GET", url, headers)
end

def http_post(url, body, headers = {})
  http_request("POST", url, headers, body)
end
