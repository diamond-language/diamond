#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-logger-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

assert_not_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" != *"$needle"* ]]
}

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/logger\"
$script"
}

# --- each level's own line carries its own label and the instance's tag ---
actual="$(run_case '
log = Logger.new("myapp")
log.info("starting up")
log.warn("low disk space")
log.error("connection failed")
')"
assert_contains "$actual" "INFO myapp: starting up"
count=$((count + 1))
assert_contains "$actual" "WARN myapp: low disk space"
count=$((count + 1))
assert_contains "$actual" "ERROR myapp: connection failed"
count=$((count + 1))

# --- every line is timestamped "[YYYY-MM-DD HH:MM:SS] ..." ---
actual="$(run_case '
log = Logger.new("myapp")
log.info("hi")
')"
[[ "$actual" =~ ^\[[0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\]\ INFO\ myapp:\ hi ]]
count=$((count + 1))

# --- default level (info) drops #debug ---
actual="$(run_case '
log = Logger.new("myapp")
log.debug("should not appear")
log.info("should appear")
')"
assert_not_contains "$actual" "should not appear"
count=$((count + 1))
assert_contains "$actual" "should appear"
count=$((count + 1))

# --- explicit level raises the floor -- warn drops both debug and info ---
actual="$(run_case '
log = Logger.new("myapp", "warn")
log.info("should not appear")
log.warn("should appear")
')"
assert_not_contains "$actual" "should not appear"
count=$((count + 1))
assert_contains "$actual" "should appear"
count=$((count + 1))

# --- explicit level lowers the floor -- debug now emits too ---
actual="$(run_case '
log = Logger.new("myapp", "debug")
log.debug("should appear")
')"
assert_contains "$actual" "DEBUG myapp: should appear"
count=$((count + 1))

# --- an unknown level name raises a rescuable ArgumentError ---
actual="$(run_case '
begin
  Logger.new("myapp", "verbose")
rescue e: ArgumentError
  puts("caught: #{e.message()}")
end
')"
assert_contains "$actual" "caught:"
count=$((count + 1))
assert_contains "$actual" "verbose"
count=$((count + 1))

# --- a custom output (anything with .write(value), matching
# File/TCPSocket's own convention) receives the same formatted line
# instead of stdout, with its own trailing newline added ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value)
    @lines.push(value)
  end
  def lines() = @lines
end
writer = CapturingWriter.new()
log = Logger.new("myapp", "info", writer)
log.info("to the writer")
puts(writer.lines().length())
puts(writer.lines()[0])
')"
assert_contains "$actual" "1"
count=$((count + 1))
assert_contains "$actual" "INFO myapp: to the writer"
count=$((count + 1))

# --- JSON format emits one parseable object with structured fields and
# correct escaping; base metadata wins over conflicting caller fields ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
  def lines() = @lines
end
writer = CapturingWriter.new()
log = Logger.new("myapp", "debug", writer, "json")
log.info("user signed in \"safely\"", {"user_id": 7, "ok": true, "level": "fake"})
record = JSON.parse(writer.lines()[0])
puts("#{record["level"]}|#{record["tag"]}|#{record["message"]}|#{record["user_id"]}|#{record["ok"]}|#{record["timestamp"].length() > 0}")
')"
assert_contains "$actual" 'info|myapp|user signed in "safely"|7|true|true'
count=$((count + 1))

# --- bad formats fail at construction rather than silently falling back ---
actual="$(run_case '
begin
  Logger.new("myapp", "info", nil, "xml")
rescue e: ArgumentError
  puts(e.message())
end
')"
assert_contains "$actual" "unknown format 'xml'"
count=$((count + 1))

# --- off is a level above every emitted severity, so callers retain their
# instrumentation while serialization and output become a no-op ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
  def lines() = @lines
end
writer = CapturingWriter.new()
log = Logger.new("myapp", "off", writer, "json")
log.debug("debug", {"duration_ms": 0.1})
log.info("info")
log.warn("warn")
log.error("error")
writer.lines().length()
')"
[[ "$actual" == "0" ]]
count=$((count + 1))

# --- RequestLogging.call: mints request_id, logs request.started then
# --- request.completed (with status/duration_ms) around a successful
# --- forward, and clears context["log_context"] afterward ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
  def lines() = @lines
end
writer = CapturingWriter.new()
RequestLogging.configure(tag: "app", level: "debug", output: writer, format: "json")
def handler(request, context) = [200, {}, "ok"]
request = {"method": "GET", "path": "/hello"}
context = {}
[status, headers, body] = RequestLogging.call(request, context, handler)
"#{status}|#{request["request_id"] != nil}|#{writer.lines().length()}|#{writer.lines()[0].index_of("request.started") != nil}|#{writer.lines()[1].index_of("request.completed") != nil}|#{writer.lines()[1].index_of("\"status\":200") != nil}|#{context["log_context"]}"
')"
[[ "$actual" == "200|true|2|true|true|true|nil" ]]
count=$((count + 1))

# --- RequestLogging.call: a forward that raises logs request.failed
# --- (error_class/error_message/duration_ms), clears log_context, and
# --- re-raises rather than swallowing the error ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
  def lines() = @lines
end
writer = CapturingWriter.new()
RequestLogging.configure(tag: "app", level: "debug", output: writer, format: "json")
def failing_handler(request, context)
  raise RuntimeError.new("boom")
end
request = {"method": "POST", "path": "/explode"}
context = {}
begin
  RequestLogging.call(request, context, failing_handler)
  "no exception raised"
rescue error: RuntimeError
  "#{error.message()}|#{writer.lines().length()}|#{writer.lines()[1].index_of("request.failed") != nil}|#{writer.lines()[1].index_of("boom") != nil}|#{context["log_context"]}"
end
')"
[[ "$actual" == "boom|2|true|true|nil" ]]
count=$((count + 1))

# --- RequestLogging.correlation: reflects the in-flight request's
# --- fields from inside .call's own forward, and is {} once no request
# --- is in flight (context["log_context"] never set, or already
# --- cleared) ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
end
RequestLogging.configure(tag: "app", level: "info", output: CapturingWriter.new(), format: "json")
outside = RequestLogging.correlation({})
def handler_reading_correlation(request, context)
  correlation = RequestLogging.correlation(context)
  [200, {}, "method=#{correlation["method"]} path=#{correlation["path"]}"]
end
context = {}
[status, headers, body] = RequestLogging.call({"method": "GET", "path": "/x"}, context, handler_reading_correlation)
"#{outside.length()}|#{body}"
')"
[[ "$actual" == "0|method=GET path=/x" ]]
count=$((count + 1))

# --- RequestLogging.debug/.info/.warn: route through the same
# --- context-memoized Logger .get returns, tagging every call with
# --- request_id/method/path plus the caller's own fields ---
actual="$(run_case '
class CapturingWriter
  def initialize()
    @lines = []
  end
  def write(value) = @lines.push(value)
  def lines() = @lines
end
writer = CapturingWriter.new()
RequestLogging.configure(tag: "app", level: "debug", output: writer, format: "json")
request = {"request_id": "abc123", "method": "GET", "path": "/skins/1"}
context = {}
RequestLogging.debug(request, context, "authentication.cookie_absent")
RequestLogging.warn(request, context, "authorization.denied", {"reason": "not_owner"})
"#{writer.lines().length()}|#{writer.lines()[0].index_of("\"request_id\":\"abc123\"") != nil}|#{writer.lines()[1].index_of("\"reason\":\"not_owner\"") != nil}"
')"
[[ "$actual" == "2|true|true" ]]
count=$((count + 1))

echo "$count logger tests passed"
