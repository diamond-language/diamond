# Minimal Rack-style HTTP/1.1 support for Diamond. Install via facet
# (see package.di), then `require "http"` to bring in http_serve
# (server) and http_get/http_post/http_request (client).
#
# A server request is a Hash: {"method": ..., "path": ..., "headers": ...,
# "body": ...}. A handler is a Callable[1] taking a request and
# returning a 3-element response Array: [status, headers, body]. A
# client response is a Hash too, but shaped for reading rather than
# building positionally: {"status": ..., "headers": ..., "body": ...}.
#
# Deliberately basic on both sides: no chunked transfer encoding, no
# keep-alive (every connection/request is handled/sent once then
# closed), no redirect-following on the client side, and a malformed
# request/response is not caught -- it propagates like any other
# error. The one exception: a connection that closes before sending a
# request line (a port scanner, a health check, anything that connects
# and disconnects without writing) is closed and skipped rather than
# crashing the whole server -- http_serve is a single blocking accept
# loop, so an unhandled error from one connection would otherwise take
# every subsequent connection down with it. See http_request's own
# comment for what's specific to the client half.

def http_status_text(status)
  if status == 200
    "OK"
  elsif status == 201
    "Created"
  elsif status == 204
    "No Content"
  elsif status == 301
    "Moved Permanently"
  elsif status == 302
    "Found"
  elsif status == 400
    "Bad Request"
  elsif status == 401
    "Unauthorized"
  elsif status == 403
    "Forbidden"
  elsif status == 404
    "Not Found"
  elsif status == 405
    "Method Not Allowed"
  elsif status == 500
    "Internal Server Error"
  else
    "Unknown"
  end
end

# A peer's Content-Length header is just a claim -- reading exactly
# that many bytes with no upper bound lets a malicious/misbehaving
# peer declare a multi-gigabyte body and force this "deliberately
# basic" server/client to accumulate that much memory (a request never
# even has to finish; the accumulation itself is the resource cost).
# 10 MiB is generous for the plain-text/JSON request and response
# bodies this package is meant for, comfortably below what would
# actually pressure memory even under several concurrent connections
# (http_serve is single-threaded/blocking, so this bounds one
# connection at a time, not a fleet of them).
def http_max_body_size()
  10485760
end

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
  status = response[0]
  headers = response[1]
  body = response[2]
  conn.write("HTTP/1.1 #{status} #{http_status_text(status)}\r\n")
  def write_header(name, value)
    conn.write("#{name}: #{value}\r\n")
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
