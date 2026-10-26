# minitest

A small minitest-style test library: assertions, skips, `setup`/`teardown` hooks
and a runner that reports failures and sets the exit status.

## Installation

Tests are a development dependency, so add it to `diamond.dev.cut` (see the
[package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md#development-dependencies)):

```sh
facet add minitest --dev
facet install
```

This installs the cut into `cuts/minitest/`; load it with `require_cut "minitest"`.
Projects that depend on yours do not install it.

## Usage

```ruby
require_cut "minitest"

def run_tests()
  def test_addition() = Minitest.assert_equal(4, 2 + 2)
  def test_lookup() = Minitest.assert_nil({"a": 1}["b"])

  suite = Minitest.new()
  suite.test("addition works", test_addition)
  suite.test("missing keys are nil", test_lookup)
  exit(suite.run() ? 0 : 1)
end
run_tests()
```

```text
2 tests, 2 passed, 0 failed, 0 errors, 0 skipped
```

Each test is a `def` nested inside another `def`: only a nested `def` is a value
that can be passed to `suite.test`. A failure prints `FAIL: name` and the
assertion's message before the summary, and `run()` returns `false`.

## API

Assertions are class methods on `Minitest`. Each returns `true` or raises
`AssertionError`; the optional last argument replaces the failure message.

- `assert(condition)`, `refute(condition)`
- `assert_equal(expected, actual)`, `refute_equal(expected, actual)`
- `assert_nil(value)`, `refute_nil(value)`
- `assert_includes(array, item)`, `refute_includes(array, item)`
- `assert_empty(collection)`, `refute_empty(collection)`
- `assert_in_delta(expected, actual, delta = 0.001)`
- `assert_raises[ErrorClass](callable)`: passes when calling `callable` raises
  `ErrorClass` or a subclass
- `skip(reason)`: marks the running test skipped

A suite (`Minitest.new()`) registers tests with `test(name, callable)` and hooks
with `setup(callable)` and `teardown(callable)`; teardown runs even when a test
fails. `run()` runs every test, prints the report and returns whether all of
them passed (skips do not count against it). `run!()` raises instead of returning
`false`, so an uncaught failure exits nonzero.
