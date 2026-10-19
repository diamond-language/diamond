# Probe registry HTTPS health and optionally a pinned archive; print one JSON record.
#
#   diamond monitor.di BASE_URL [--ca-file FILE] [--timeout SECONDS] [--archive-sha256 DIGEST]
#
# Exits 0 when the probe passed and 1 when it failed. The record carries only an error class
# (ValueError, OSError or Timeout), never exception text, which can contain URLs or
# remote-controlled contents.
#
# --timeout bounds each socket operation, not the whole probe: a server that sends one byte
# just inside it keeps a single read going for as long as it likes. The probe therefore runs
# in a worker thread, and the whole run is cut off after twice --timeout plus 5 seconds (the
# health check and the archive each get --timeout) with the error class Timeout.

def usage(message)
  warn(message)
  exit(2)
end

# [host, port, path prefix] for an HTTPS base URL without credentials, query or fragment.
def parse_base(base)
  unless base.start_with?("https://")
    raise ArgumentError.new("expected an HTTPS base URL without credentials, query or fragment")
  end
  rest = base[8, base.length() - 8]
  if rest.include?("@") || rest.include?("?") || rest.include?("#")
    raise ArgumentError.new("expected an HTTPS base URL without credentials, query or fragment")
  end
  slash = rest.index("/")
  authority = slash == nil ? rest : rest[0, slash]
  path = slash == nil ? "" : rest[slash, rest.length() - slash]
  host = authority
  port = 443
  colon = authority.index(":")
  if colon != nil
    host = authority[0, colon]
    port = authority[colon + 1, authority.length() - colon - 1].to_i()
  end
  raise ArgumentError.new("expected an HTTPS base URL") if host == ""
  prefix = path
  while prefix.end_with?("/")
    prefix = prefix[0, prefix.length() - 1]
  end
  [host, port, prefix]
end

# One GET over a fresh TLS connection. Returns [status, socket]; the caller reads the body
# (`length` bytes) and closes the socket.
def get(host, port, path, ca_file, timeout)
  options = {"read_timeout_ms": timeout * 1000, "write_timeout_ms": timeout * 1000,
             "connect_timeout_ms": timeout * 1000}
  options["ca_file"] = ca_file if ca_file != nil
  socket = TLSSocket.connect(host, port, options)
  socket.write("GET #{path} HTTP/1.1\r\nHost: #{host}\r\nConnection: close\r\n\r\n")
  status_line = socket.gets()
  raise IOError.new("no response") if status_line == nil
  status = status_line.split(" ")[1].to_i()
  length = nil
  loop do
    header = socket.gets()
    break if header == nil || header.strip() == ""
    lowered = header.downcase()
    if lowered.start_with?("content-length:")
      length = header.split(":")[1].strip().to_i()
    elsif lowered.start_with?("transfer-encoding:") && lowered.include?("chunked")
      raise ArgumentError.new("chunked responses are not supported")
    end
  end
  [status, socket, length]
end

def probe(base, ca_file, timeout, archive)
  target = parse_base(base)
  host = target[0]
  port = target[1]
  prefix = target[2]

  health_response = get(host, port, prefix + "/health", ca_file, timeout)
  status = health_response[0]
  socket = health_response[1]
  length = health_response[2]
  begin
    body = length == nil ? socket.read(4097) : socket.read(length > 4097 ? 4097 : length)
  ensure
    socket.close()
  end
  body = "" if body == nil
  raise ArgumentError.new("health response failed") if status != 200 || body.length() > 4096
  health = JSON.parse(body)
  unless health.is_a?(Hash) && health["protocol"] == 1 && health["status"] == "ok"
    raise ArgumentError.new("unexpected health response")
  end

  if archive != nil
    response = get(host, port, prefix + "/v1/blobs/sha256/" + archive, ca_file, timeout)
    raise ArgumentError.new("archive response failed") if response[0] != 200
    blob_socket = response[1]
    pieces = []
    total = 0
    started = Time.monotonic()
    begin
      loop do
        block = blob_socket.read(65536)
        break if block == nil || block == ""
        total += block.length()
        if total > 58720256 || Time.monotonic() - started > timeout
          raise ArgumentError.new("archive probe limit exceeded")
        end
        pieces.push(block)
      end
    ensure
      blob_socket.close()
    end
    raise ArgumentError.new("archive digest mismatch") unless Digest.sha256(pieces.join()) == archive
  end
end

base = nil
ca_file = nil
timeout = 10
archive = nil
args = ARGV
index = 0
while index < args.length()
  arg = args[index]
  if arg == "--ca-file"
    index += 1
    usage("--ca-file needs a value") if index >= args.length()
    ca_file = args[index]
  elsif arg == "--timeout"
    index += 1
    usage("--timeout needs a value") if index >= args.length()
    timeout = args[index].to_i()
    usage("--timeout must be from 1 to 300") if timeout < 1 || timeout > 300
  elsif arg == "--archive-sha256"
    index += 1
    usage("--archive-sha256 needs a value") if index >= args.length()
    archive = args[index]
    unless archive.length() == 64 && Regexp.new("\\A[0-9a-f]{64}\\z").match?(archive)
      usage("archive digest must be 64 lowercase hex characters")
    end
  elsif base == nil && !arg.start_with?("--")
    base = arg
  else
    usage("unexpected argument #{arg}")
  end
  index += 1
end
usage("usage: monitor.di BASE_URL [--ca-file FILE] [--timeout SECONDS] [--archive-sha256 DIGEST]") if base == nil

# Runs the probe and reports its outcome as a String on `results`. A top-level def with arguments only, so it can start a Thread.
def probe_worker(base, ca_file, timeout, archive, results)
  outcome = "ok"
  begin
    probe(base, ca_file, timeout, archive)
  rescue e: ArgumentError
    outcome = "ValueError"
  rescue e: StandardError
    outcome = "OSError"
  end
  results.send(outcome)
end

started = Time.monotonic()
results = Channel.new(1)
worker = Thread.new(probe_worker, base, ca_file, timeout, archive, results)
deadline = started + timeout * 2 + 5
outcome = nil
while outcome == nil
  if results.size() > 0
    outcome = results.receive()
  elsif Time.monotonic() >= deadline
    # The worker may still be blocked in a read; exiting below ends it with the process.
    outcome = "Timeout"
  else
    results.wait_readable([], deadline)
  end
end
result = if outcome == "ok" then {"ok": true} else {"ok": false, "error": outcome} end
elapsed = ((Time.monotonic() - started) * 1000).round()
result["duration_ms"] = elapsed
puts(JSON.stringify(result))
exit(result["ok"] ? 0 : 1)
