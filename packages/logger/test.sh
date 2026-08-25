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

echo "$count logger tests passed"
