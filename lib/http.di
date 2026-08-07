# Minimal Rack-style HTTP/1.1 support. Not embedded in every program like
# lib/core.di -- require "lib/http" to opt in.
#
# A request is a Hash: {"method": ..., "path": ..., "headers": ..., "body": ...}.
# A handler is a Callable[1] taking a request and returning a 3-element
# response Array: [status, headers, body].
#
# Deliberately basic: no chunked transfer encoding, no keep-alive (every
# connection is handled once then closed), and a bad or malformed request
# is not caught -- it propagates out of http_serve like any other error.

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

def http_parse_request(conn)
  request_line = conn.gets()
  method_end = request_line.index_of(" ")
  method = request_line.slice(0, method_end)
  after_method = request_line.slice(method_end + 1, request_line.length())
  path_end = after_method.index_of(" ")
  path = after_method.slice(0, path_end)

  headers = {}
  def collect_header(line)
    colon = line.index_of(": ")
    if colon != nil
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
  if content_length != nil
    body = conn.read(content_length.to_i())
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
    response = handler(request)
    http_write_response(conn, response)
    conn.close()
  end
end
