#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (`make test-minitest-package` from the repo root sets this up).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

# The cut ships the repository's lib/minitest.d6, which the repository's own
# tests use; the two must not drift apart.
cmp -s lib/minitest.d6 ../../lib/minitest.d6 || {
    echo "packages/minitest/lib/minitest.d6 differs from lib/minitest.d6" >&2
    exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

run_suite() {
    printf '%s\n' "require \"$(pwd)/lib/minitest\"" "$1" > "$work/suite.d6"
    "$diamond" "$work/suite.d6"
}

# A passing suite reports its counts and run() returns true.
actual="$(run_suite '
def run_tests()
  def test_equal() = Minitest.assert_equal(4, 2 + 2)
  def raise_argument_error()
    raise ArgumentError.new("x")
  end
  def test_raises() = Minitest.assert_raises[ArgumentError](raise_argument_error)
  def test_skip() = Minitest.skip("later")
  suite = Minitest.new()
  suite.test("equal", test_equal)
  suite.test("raises", test_raises)
  suite.test("skip", test_skip)
  exit(suite.run() ? 0 : 1)
end
run_tests()')"
[[ "$actual" == *"SKIP: skip"* ]]
[[ "$actual" == *"3 tests, 2 passed, 0 failed, 0 errors, 1 skipped" ]]

# A failure and an error are reported separately, and the suite exits 1.
if actual="$(run_suite '
def run_tests()
  def test_fails() = Minitest.assert_equal("a", "b")
  def test_errors()
    raise RuntimeError.new("boom")
  end
  def make_teardown(calls)
    def teardown_hook() = calls.push(1)
    teardown_hook
  end
  calls = []
  suite = Minitest.new()
  suite.teardown(make_teardown(calls))
  suite.test("fails", test_fails)
  suite.test("errors", test_errors)
  passed = suite.run()
  puts("teardowns: #{calls.length()}")
  exit(passed ? 0 : 1)
end
run_tests()')"; then
    echo "a failing suite exited 0" >&2
    exit 1
fi
[[ "$actual" == *"FAIL: fails"* ]]
[[ "$actual" == *"ERROR: errors (RuntimeError)"* ]]
[[ "$actual" == *"2 tests, 0 passed, 1 failed, 1 error, 0 skipped"* ]]
[[ "$actual" == *"teardowns: 2"* ]]

echo "minitest package tests passed"
