# Client: http_get/http_post/http_put/http_patch/http_delete/http_head/
# http_options and the lower-level http_request all make a real outbound
# request to any host TCPSocket.connect/TLSSocket.connect can reach --
# unlike http_serve above, not limited to loopback/localhost in any way.
# http:// and https:// are both supported (the earlier "http only,
# https:// raises" restriction is gone now that TLSSocket.connect can do
# everything an HTTP client needs -- see docs/io.md's `options` Hash).
#
# `options` (the last argument to every function here, always optional,
# always a Hash) controls everything beyond the bare wire protocol:
#
# - connect_timeout_ms/read_timeout_ms/write_timeout_ms -- forwarded
#   straight to TCPSocket.connect/TLSSocket.connect.
# - ca_file/ca_path/cert/key -- forwarded to TLSSocket.connect for an
#   https:// URL (a custom trust store, or a client certificate for
#   mutual TLS); ignored for http://.
# - gzip -- true by default: sends "Accept-Encoding: gzip, deflate" and
#   transparently decompresses a response whose Content-Encoding says
#   either. Set to false to send/receive the wire bytes as-is.
# - basic_auth: {"user":, "password":} -- sets "Authorization: Basic
#   <base64>". bearer_token: "..." -- sets "Authorization: Bearer ...".
#   Both are overridden by an explicit "Authorization" header if the
#   caller also passes one (see http_prepare_headers_and_body below).
# - json: <any value> -- the request body becomes JSON.stringify(value)
#   and Content-Type becomes "application/json" (unless the caller's own
#   headers already set one). Response bodies are never auto-parsed as
#   JSON -- the caller already has the response Hash and JSON.parse
#   right there; auto-parsing would need to guess at a Content-Type the
#   server might not even send correctly.
# - multipart_fields: {name: value, ...} / multipart_files: {name:
#   {"filename":, "content_type":, "data":}, ...} -- the request body
#   becomes a multipart/form-data encoding of both (see
#   http_encode_multipart below), for file uploads. Content-Type
#   (including the boundary) is set the same way json's is.
# - follow_redirects -- false by default (matching this package's
#   existing "no magic unless asked" stance); true follows a 3xx
#   response's own Location header, up to max_redirects (default 5)
#   hops, raising IOError past that rather than looping forever on a
#   redirect cycle. See http_follow_redirect below for the per-status
#   method/body rules.
#
# Chunked Transfer-Encoding is now decoded (http_read_chunked_body),
# unlike this package's server half, which still doesn't emit it.
# HttpSession (bottom of this file) adds a cookie jar and per-host
# connection reuse across multiple calls; the plain functions above
# open and close one connection per call, exactly as before.

def http_parse_url(url)
  scheme_end = url.index_of("://")
  if scheme_end == nil
    raise ArgumentError.new("invalid URL (missing scheme): #{url}")
  end
  scheme = url.slice(0, scheme_end)
  if scheme != "http" && scheme != "https"
    raise ArgumentError.new("unsupported URL scheme '#{scheme}' (only http/https are supported)")
  end
  rest = url.slice(scheme_end + 3, url.length())
  path_start = rest.index_of("/")
  host_and_port = rest
  path = "/"
  unless path_start == nil
    host_and_port = rest.slice(0, path_start)
    path = rest.slice(path_start, rest.length())
  end
  default_port = if scheme == "https" then 443 else 80 end
  colon = host_and_port.index_of(":")
  host = host_and_port
  port = default_port
  unless colon == nil
    host = host_and_port.slice(0, colon)
    port = host_and_port.slice(colon + 1, host_and_port.length()).to_i()
  end
  {"scheme": scheme, "host": host, "port": port, "path": path, "default_port": default_port}
end

# Case-insensitive header lookup -- HTTP header names are
# case-insensitive per spec, and outbound request headers here are
# otherwise written to the wire exactly as the caller spelled them (no
# normalization, same stance http_serve's own response-header handling
# takes), so checking "did the caller already set X" has to search by
# name rather than assume one canonical casing.
def http_header_lookup(headers, name)
  target = name.downcase()
  found = nil
  def check_header(key, value)
    if key.downcase() == target
      found = value
    end
  end
  headers.each(check_header)
  found
end

# Builds a random multipart/form-data boundary and body from `fields`
# (name -> String value) and `files` (name -> {"filename":,
# "content_type":, "data":}) -- the exact shapes multipart_parse
# (packages/multipart) produces on the server side, kept as a small
# self-contained encoder here rather than a new dependency on that
# package: multipart's own scope is deliberately parse-only (server-side
# request bodies), and an encoder is a different enough concern (~20
# lines) that coupling the two packages together over facet isn't worth
# it for this.
def http_encode_multipart(fields, files)
  boundary = "----DiamondFormBoundary#{SecureRandom.hex(16)}"
  builder = StringBuilder.new()
  def append_field(name, value)
    builder.append("--#{boundary}\r\n")
    builder.append("Content-Disposition: form-data; name=\"#{name}\"\r\n\r\n")
    builder.append("#{value}\r\n")
  end
  fields.each(append_field)
  def append_file(name, file)
    content_type = file["content_type"]
    content_type = if content_type == nil then "application/octet-stream" else content_type end
    builder.append("--#{boundary}\r\n")
    builder.append("Content-Disposition: form-data; name=\"#{name}\"; filename=\"#{file["filename"]}\"\r\n")
    builder.append("Content-Type: #{content_type}\r\n\r\n")
    builder.append("#{file["data"]}\r\n")
  end
  files.each(append_file)
  builder.append("--#{boundary}--\r\n")
  {"content_type": "multipart/form-data; boundary=#{boundary}", "body": builder.to_s()}
end

# Resolves `options`' higher-level fields (json/multipart/auth/gzip) into
# a final {headers, body} pair. An explicit header the caller already
# set always wins over anything this function would otherwise add --
# `defaults.merge(headers)` means a same-named key in `headers` (Hash#merge's
# own "other" argument) overrides the default rather than the reverse.
def http_prepare_headers_and_body(headers, body, options)
  defaults = {}
  json_value = options["json"]
  unless json_value == nil
    body = JSON.stringify(json_value)
    defaults["Content-Type"] = "application/json"
  end
  multipart_fields = options["multipart_fields"]
  multipart_files = options["multipart_files"]
  if multipart_fields != nil || multipart_files != nil
    fields = if multipart_fields == nil then {} else multipart_fields end
    files = if multipart_files == nil then {} else multipart_files end
    encoded = http_encode_multipart(fields, files)
    body = encoded["body"]
    defaults["Content-Type"] = encoded["content_type"]
  end
  basic_auth = options["basic_auth"]
  unless basic_auth == nil
    credentials = "#{basic_auth["user"]}:#{basic_auth["password"]}"
    defaults["Authorization"] = "Basic #{Base64.encode(credentials)}"
  end
  bearer_token = options["bearer_token"]
  unless bearer_token == nil
    defaults["Authorization"] = "Bearer #{bearer_token}"
  end
  gzip_option = options["gzip"]
  if gzip_option == nil || gzip_option == true
    if http_header_lookup(headers, "Accept-Encoding") == nil
      defaults["Accept-Encoding"] = "gzip, deflate"
    end
  end
  {"headers": defaults.merge(headers), "body": body}
end

# Only the socket-level options apply to a plain http:// connection --
# ca_file/ca_path/cert/key make no sense without a TLS layer at all.
def http_socket_options(options)
  socket_options = {}
  connect_timeout = options["connect_timeout_ms"]
  unless connect_timeout == nil
    socket_options["connect_timeout_ms"] = connect_timeout
  end
  read_timeout = options["read_timeout_ms"]
  unless read_timeout == nil
    socket_options["read_timeout_ms"] = read_timeout
  end
  write_timeout = options["write_timeout_ms"]
  unless write_timeout == nil
    socket_options["write_timeout_ms"] = write_timeout
  end
  socket_options
end

def http_tls_options(options)
  tls_options = http_socket_options(options)
  ca_file = options["ca_file"]
  unless ca_file == nil
    tls_options["ca_file"] = ca_file
  end
  ca_path = options["ca_path"]
  unless ca_path == nil
    tls_options["ca_path"] = ca_path
  end
  cert = options["cert"]
  unless cert == nil
    tls_options["cert"] = cert
  end
  key = options["key"]
  unless key == nil
    tls_options["key"] = key
  end
  tls_options
end

def http_connect(scheme, host, port, options)
  if scheme == "https"
    TLSSocket.connect(host, port, http_tls_options(options))
  else
    TCPSocket.connect(host, port, http_socket_options(options))
  end
end

# Hex chunk-size lines (Transfer-Encoding: chunked) have no native String
# radix parse to lean on (String#to_i is base-10 only) -- small enough to
# hand-roll rather than pull in a general base-N parser for one call
# site.
def http_parse_hex(text)
  value = 0
  index = 0
  while index < text.length()
    code = text[index].ord()
    digit = if code >= 48 && code <= 57
      code - 48
    elsif code >= 97 && code <= 102
      code - 97 + 10
    elsif code >= 65 && code <= 70
      code - 65 + 10
    else
      raise IOError.new("invalid chunk size in chunked response body")
    end
    value = value * 16 + digit
    index = index + 1
  end
  value
end

# Decodes a Transfer-Encoding: chunked body: each chunk is a hex length
# line, exactly that many bytes, then a bare CRLF, repeating until a
# zero-length chunk -- optionally followed by trailer headers, consumed
# and discarded here (nothing in this package's response Hash surfaces
# trailers; a real consumer of them is a real, currently-unneeded
# feature). Bounded by the same http_max_body_size() the Content-Length
# path already enforces, checked per chunk rather than after accumulating
# an oversized body.
def http_read_chunked_body(conn)
  builder = StringBuilder.new()
  loop do
    size_line = conn.gets()
    if size_line == nil
      raise IOError.new("connection closed while reading chunked response body")
    end
    semicolon = size_line.index_of(";")
    size_text = if semicolon == nil then size_line else size_line.slice(0, semicolon) end
    chunk_size = http_parse_hex(size_text.strip())
    if chunk_size == 0
      loop do
        trailer = conn.gets()
        if trailer == nil || trailer == ""
          break
        end
      end
      break
    end
    if builder.length() + chunk_size > http_max_body_size()
      raise IOError.new("chunked response body exceeds maximum of #{http_max_body_size()}")
    end
    builder.append(conn.read(chunk_size))
    conn.gets() # the bare CRLF following each chunk's data
  end
  builder.to_s()
end

# Reads and returns the raw (still possibly gzip/deflate-encoded) body
# for a response already past its status line and headers, choosing
# among chunked/Content-Length/read-to-EOF exactly as before -- this is
# the one piece of http_request's old body-reading logic, unchanged
# apart from also recognizing chunked encoding.
def http_read_body(conn, response_headers)
  if response_headers["transfer-encoding"] == "chunked"
    return http_read_chunked_body(conn)
  end
  content_length = response_headers["content-length"]
  if content_length != nil
    length = content_length.to_i()
    if length > http_max_body_size()
      conn.close()
      raise IOError.new("response Content-Length #{length} exceeds maximum of #{http_max_body_size()}")
    end
    return conn.read(length)
  end
  conn.read()
end

# Undoes Content-Encoding: gzip/deflate (Gzip.decompress auto-detects
# either wire format -- see docs/syntax.md) when the response declares
# one and the caller hasn't opted out via options["gzip"] = false.
# Anything else in Content-Encoding (identity, or a scheme this package
# doesn't implement, e.g. br/zstd) is left exactly as received -- a
# caller asking for one of those explicitly via a custom header is
# responsible for decoding it themselves.
def http_maybe_decompress(body, response_headers, options)
  gzip_option = options["gzip"]
  if gzip_option == false
    return body
  end
  encoding = response_headers["content-encoding"]
  if encoding == "gzip" || encoding == "x-gzip" || encoding == "deflate"
    return Gzip.decompress(body, http_max_body_size())
  end
  body
end

# Writes one request and reads back one response over an already-open
# `conn` -- doesn't open or close anything itself, so both the plain
# one-shot functions below and HttpSession's connection-reuse path can
# share it. `host` is used for both the Host header and TLS SNI (already
# baked into `conn` by the time this runs); a non-default port is
# appended to Host, matching what a browser sends.
def http_send(conn, method, parsed, headers, body)
  host_header = parsed["host"]
  unless parsed["port"] == parsed["default_port"]
    host_header = "#{host_header}:#{parsed["port"]}"
  end
  conn.write("#{method} #{parsed["path"]} HTTP/1.1\r\n")
  conn.write("Host: #{host_header}\r\n")
  def write_request_header(name, value)
    conn.write("#{name}: #{value}\r\n")
  end
  headers.each(write_request_header)
  if body.length() > 0
    conn.write("Content-Length: #{body.length()}\r\n")
  end
  if http_header_lookup(headers, "Connection") == nil
    conn.write("Connection: close\r\n")
  end
  conn.write("\r\n")
  if body.length() > 0
    conn.write(body)
  end

  status_line = conn.gets()
  if status_line == nil
    raise IOError.new("connection closed before sending a response")
  end
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

  response_body = if method == "HEAD" then "" else http_read_body(conn, response_headers) end
  {"status": status, "headers": response_headers, "body": response_body}
end

# Redirect-following method/body rules (RFC 7231 section 6.4, as every
# real client actually implements it rather than the letter of the much
# older RFC 2616 wording): 307/308 preserve the original method and
# body exactly ("temporary"/"permanent redirect, same request again
# elsewhere"); everything else in the 3xx range this function follows
# (301/302/303) downgrades to a bodyless GET unless the original request
# was already GET/HEAD, matching curl -L and every browser rather than
# a strict reading of the spec (which technically leaves 301/302 body
# handling to the client). Authorization is dropped when the redirect
# target's host differs from the original request's -- a credential
# meant for one server has no business being replayed against whatever
# host a 3xx response points at, a known real vulnerability class
# (a compromised or malicious server redirecting to itself an
# Authorization header meant for someone else).
def http_should_follow(status)
  status == 301 || status == 302 || status == 303 || status == 307 || status == 308
end

def http_next_request_for_redirect(status, method, body, original_host, target_host, headers)
  next_headers = headers
  unless target_host == original_host
    stripped = {}
    def keep_non_auth(name, value)
      unless name.downcase() == "authorization"
        stripped[name] = value
      end
    end
    headers.each(keep_non_auth)
    next_headers = stripped
  end
  if status == 307 || status == 308
    {"method": method, "body": body, "headers": next_headers}
  elsif method == "GET" || method == "HEAD"
    {"method": method, "body": "", "headers": next_headers}
  else
    {"method": "GET", "body": "", "headers": next_headers}
  end
end

def http_request(method, url, headers = {}, body = "", options = {})
  parsed = http_parse_url(url)
  prepared = http_prepare_headers_and_body(headers, body, options)
  conn = http_connect(parsed["scheme"], parsed["host"], parsed["port"], options)
  response = http_send(conn, method, parsed, prepared["headers"], prepared["body"])
  conn.close()
  response["body"] = http_maybe_decompress(response["body"], response["headers"], options)

  follow = options["follow_redirects"]
  location = response["headers"]["location"]
  if follow == true && http_should_follow(response["status"]) && location != nil
    remaining = options["max_redirects"]
    remaining = if remaining == nil then 5 else remaining end
    if remaining <= 0
      raise IOError.new("too many redirects following #{url}")
    end
    next_url = if location.index_of("://") == nil
      "#{parsed["scheme"]}://#{parsed["host"]}:#{parsed["port"]}#{location}"
    else
      location
    end
    next_parsed = http_parse_url(next_url)
    next_request = http_next_request_for_redirect(response["status"], method,
      prepared["body"], parsed["host"], next_parsed["host"], headers)
    next_options = options.merge({"max_redirects": remaining - 1})
    return http_request(next_request["method"], next_url, next_request["headers"],
      next_request["body"], next_options)
  end
  response
end

def http_get(url, headers = {}, options = {})
  http_request("GET", url, headers, "", options)
end

def http_post(url, body = "", headers = {}, options = {})
  http_request("POST", url, headers, body, options)
end

def http_put(url, body = "", headers = {}, options = {})
  http_request("PUT", url, headers, body, options)
end

def http_patch(url, body = "", headers = {}, options = {})
  http_request("PATCH", url, headers, body, options)
end

def http_delete(url, headers = {}, options = {})
  http_request("DELETE", url, headers, "", options)
end

def http_head(url, headers = {}, options = {})
  http_request("HEAD", url, headers, "", options)
end

def http_options(url, headers = {}, options = {})
  http_request("OPTIONS", url, headers, "", options)
end

# HttpSession: a cookie jar plus one reused connection per host:port,
# for a caller making several calls against the same server (the
# natural next step once a single one-shot http_get/http_post call isn't
# enough -- logging in then making authenticated follow-up requests,
# or just avoiding a fresh TCP/TLS handshake per call).
#
# Cookie scope is deliberately simplified: a Set-Cookie name=value pair
# is stored keyed by the exact request host and sent back on every
# later request to that exact host -- no Domain-attribute subdomain
# matching, no Path scoping, no Expires/Max-Age eviction. A cookie's
# other attributes (HttpOnly, Secure, SameSite) are parsed past but not
# enforced client-side, same as this package's http_serve half already
# doesn't enforce Secure on the server side. This covers the common
# "log in, then reuse the session cookie" case; a real cookie-attribute
# implementation is a real, deliberate scope cut, not an oversight.
#
# Connection reuse is "try the cached connection, and if a write/read
# against it fails for any reason, open a fresh one and retry the
# request exactly once" -- rather than trying to track whether the
# *server* actually intends to keep it alive (parsing Connection:
# close, an idle timeout, ...). A server that already closed an idle
# connection makes the first attempt fail with an ordinary IOError; the
# retry is indistinguishable from a first attempt on a cold connection.
class HttpSession
  def initialize(options = {})
    @options = options
    @cookies = {}
    @connections = {}
  end

  def http_apply_cookies(headers, host)
    jar = @cookies[host]
    if jar == nil
      return headers
    end
    pairs = []
    def collect_pair(name, value)
      pairs << "#{name}=#{value}"
    end
    jar.each(collect_pair)
    merged = headers.merge({})
    existing = http_header_lookup(headers, "Cookie")
    cookie_line = pairs.join("; ")
    if existing != nil
      cookie_line = "#{existing}; #{cookie_line}"
    end
    merged["Cookie"] = cookie_line
    merged
  end

  # A Set-Cookie header may repeat per response; this package's own
  # header collection (http_send's collect_response_header) keeps only
  # the last of any repeated header name, so multi-cookie responses are
  # a known, documented gap here rather than a silent one -- a session
  # that needs more than one Set-Cookie per response needs a raw header
  # list this response shape doesn't carry.
  def http_store_cookie(host, set_cookie_line)
    if set_cookie_line == nil
      return nil
    end
    semicolon = set_cookie_line.index_of(";")
    pair = if semicolon == nil then set_cookie_line else set_cookie_line.slice(0, semicolon) end
    equals = pair.index_of("=")
    if equals == nil
      return nil
    end
    name = pair.slice(0, equals).strip()
    value = pair.slice(equals + 1, pair.length()).strip()
    jar = @cookies[host]
    if jar == nil
      jar = {}
      @cookies[host] = jar
    end
    jar[name] = value
    nil
  end

  def connection_key(parsed)
    "#{parsed["scheme"]}://#{parsed["host"]}:#{parsed["port"]}"
  end

  def request(method, url, headers = {}, body = "")
    parsed = http_parse_url(url)
    with_cookies = self.http_apply_cookies(headers, parsed["host"])
    prepared = http_prepare_headers_and_body(with_cookies, body, @options)
    request_headers = prepared["headers"]
    if http_header_lookup(request_headers, "Connection") == nil
      request_headers = request_headers.merge({"Connection": "keep-alive"})
    end
    key = self.connection_key(parsed)
    cached = @connections[key]
    response = nil
    if cached == nil
      conn = http_connect(parsed["scheme"], parsed["host"], parsed["port"], @options)
      response = http_send(conn, method, parsed, request_headers, prepared["body"])
      @connections[key] = conn
    else
      begin
        response = http_send(cached, method, parsed, request_headers, prepared["body"])
      rescue error: IOError
        conn = http_connect(parsed["scheme"], parsed["host"], parsed["port"], @options)
        response = http_send(conn, method, parsed, request_headers, prepared["body"])
        @connections[key] = conn
      end
    end
    response["body"] = http_maybe_decompress(response["body"], response["headers"], @options)
    self.http_store_cookie(parsed["host"], response["headers"]["set-cookie"])

    follow = @options["follow_redirects"]
    location = response["headers"]["location"]
    if follow == true && http_should_follow(response["status"]) && location != nil
      next_url = if location.index_of("://") == nil
        "#{parsed["scheme"]}://#{parsed["host"]}:#{parsed["port"]}#{location}"
      else
        location
      end
      next_parsed = http_parse_url(next_url)
      next_request = http_next_request_for_redirect(response["status"], method,
        prepared["body"], parsed["host"], next_parsed["host"], headers)
      return self.request(next_request["method"], next_url, next_request["headers"], next_request["body"])
    end
    response
  end

  def get(url, headers = {})
    self.request("GET", url, headers, "")
  end

  def post(url, body = "", headers = {})
    self.request("POST", url, headers, body)
  end

  def put(url, body = "", headers = {})
    self.request("PUT", url, headers, body)
  end

  def patch(url, body = "", headers = {})
    self.request("PATCH", url, headers, body)
  end

  def delete(url, headers = {})
    self.request("DELETE", url, headers, "")
  end

  def close()
    def close_one(key, conn)
      conn.close()
    end
    @connections.each(close_one)
    @connections = {}
  end
end
