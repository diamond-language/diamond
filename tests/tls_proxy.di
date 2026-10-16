# A tiny HTTPS front for a plain-HTTP backend, standing in for nginx in tests.
#
#   diamond tls_proxy.di CERT KEY BACKEND_PORT
#
# Prints `port N` once it is listening. One request per connection: the request is forwarded with
# `Connection: close`, the backend's whole reply is relayed, and the connection is closed.
cert = ARGV[0]
key = ARGV[1]
backend_port = ARGV[2].to_i()

def forward(client, backend_port)
  request_line = client.gets()
  return nil if request_line == nil
  headers = []
  length = 0
  expects_continue = false
  loop do
    line = client.gets()
    break if line == nil || line.strip() == ""
    lowered = line.downcase()
    next if lowered.start_with?("connection:")
    length = line.split(":")[1].strip().to_i() if lowered.start_with?("content-length:")
    expects_continue = true if lowered.start_with?("expect:") && lowered.include?("100-continue")
    headers.push(line.strip())
  end
  client.write("HTTP/1.1 100 Continue\r\n\r\n") if expects_continue
  body = length > 0 ? client.read(length) : ""
  backend = TCPSocket.connect("127.0.0.1", backend_port,
    {"connect_timeout_ms": 5000, "read_timeout_ms": 30000, "write_timeout_ms": 30000})
  begin
    backend.write(request_line.strip() + "\r\n" + headers.join("\r\n") + "\r\nConnection: close\r\n\r\n" + (body == nil ? "" : body))
    backend.flush()
    client.write(backend.read())
  ensure
    backend.close()
  end
end

listener = TLSServer.listen(0, cert, key)
puts("port #{listener.port()}")
loop do
  client = nil
  begin
    client = listener.accept()
  rescue e: IOError
    next
  end
  begin
    forward(client, backend_port)
  rescue e: StandardError
    nil
  ensure
    client.close()
  end
end
