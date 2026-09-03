# A minimal, self-hosted minitest/RSpec-style testing library for Diamond
# programs. Not part of the lib/core.di prelude -- require it explicitly
# so ordinary programs (and every tests/cases/*.di file) don't pay for
# it on every compile:
#
#   require "../lib/minitest"
#
#   def run_tests()
#     def test_addition()
#       Minitest.assert_equal(4, 2 + 2)
#     end
#
#     suite = Minitest.new()
#     suite.test("addition works", test_addition)
#     suite.run!()
#   end
#   run_tests()
#
# Diamond has runtime type introspection via `class()`/`is_a?`, but no
# dynamic dispatch by string name or first-class Class values (see
# docs/roadmap.md) -- so tests
# can't be auto-discovered by scanning for a `test_` prefix the way real
# minitest does; each test is registered explicitly via
# `test(name, callback)` instead. Each test function also has to be
# *nested* inside an enclosing `def` (here, `run_tests`), not top-level:
# a top-level `def` is only ever callable by name (`foo()`), never
# usable as a value -- only a nested `def` compiles to a real closure a
# variable can hold and pass around, the same restriction every
# `Callable`-typed helper in lib/core.di and tests/cases/ already lives
# with.
#
# assert_raises *can* check for a specific exception type, via an
# explicit generic type argument -- `assert_raises[SomeError](action)`
# -- even though Diamond has no first-class Class values: `rescue`
# clauses can filter on a bound generic type parameter (E in
# `def self.assert_raises[E](...)`), resolved against the type argument
# given at the call site.
#
# Assertions raise AssertionError on failure and return true on
# success, exactly like real minitest's assertions -- `run` is what
# catches AssertionError (recorded as a failure) separately from any
# other StandardError (recorded as an error), a raised Skip (recorded as
# a skip, via `Minitest.skip`), and anything else (no exception) as a
# pass.
#
# Optional `setup`/`teardown` hooks (each a zero-argument Callable,
# registered once per suite -- calling `.setup`/`.teardown` again
# replaces the previous hook rather than adding a second one, matching
# how `.test` for the same name would just register a duplicate rather
# than erroring: this library favors simple, predictable overwrite
# semantics over guarding against a caller's own mistake) run
# immediately before/after *every* registered test, teardown via
# `ensure` so it still runs even when the test failed, errored, or was
# skipped.

class AssertionError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

# Raised by Minitest.skip to mark the current test skipped rather than
# failed or errored -- a separate class from AssertionError specifically
# so `run` can tell "this assertion failed" apart from "this test asked
# not to run" and bucket/report them differently.
class Skip < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

class Minitest
  def initialize()
    @names = []
    @callbacks = []
    @passed = 0
    @failed = 0
    @errored = 0
    @skipped = 0
    @failures = []
    @setup_hook = nil
    @teardown_hook = nil
  end

  attr_reader passed: Int
  attr_reader failed: Int
  attr_reader errored: Int
  attr_reader skipped: Int
  attr_reader failures: Array

  def self.assert(condition: Bool, message: String = "assertion failed") -> Bool
    unless condition
      raise AssertionError.new(message)
    end
    true
  end

  def self.refute(condition: Bool, message: String = "expected condition to be false") -> Bool
    Minitest.assert(!condition, message)
  end

  def self.assert_equal(expected, actual, message: String = "") -> Bool
    if expected != actual
      prefix = if message == "" then "" else message + "\n" end
      raise AssertionError.new("#{prefix}expected #{JSON.stringify(expected)}, got #{JSON.stringify(actual)}")
    end
    true
  end

  def self.refute_equal(expected, actual, message: String = "") -> Bool
    if expected == actual
      prefix = if message == "" then "" else message + "\n" end
      raise AssertionError.new("#{prefix}expected #{JSON.stringify(expected)} to differ from #{JSON.stringify(actual)}")
    end
    true
  end

  def self.assert_nil(value, message: String = "") -> Bool
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(value == nil, "#{prefix}expected nil, got #{JSON.stringify(value)}")
  end

  def self.refute_nil(value, message: String = "") -> Bool
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(value != nil, "#{prefix}expected a non-nil value")
  end

  # Array membership only -- not Hash/String -- since it's the
  # overwhelmingly common case ("did this collection end up containing
  # X") and a Hash's own two plausible meanings (does it have this key?
  # does it have this value?) don't share one obvious answer the way
  # Array's does. Checks with `==`, so it works for any element type
  # that itself supports equality, same as assert_equal.
  def self.assert_includes(collection: Array, item, message: String = "") -> Bool
    index = 0
    found = false
    while index < collection.length()
      if collection[index] == item
        found = true
      end
      index += 1
    end
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(found,
      "#{prefix}expected #{JSON.stringify(collection)} to include #{JSON.stringify(item)}")
  end

  def self.refute_includes(collection: Array, item, message: String = "") -> Bool
    index = 0
    found = false
    while index < collection.length()
      if collection[index] == item
        found = true
      end
      index += 1
    end
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(!found,
      "#{prefix}expected #{JSON.stringify(collection)} not to include #{JSON.stringify(item)}")
  end

  # Works on Array, Hash, and String alike -- all three share `.length()`,
  # so "empty" means the same thing (zero elements/characters) for each
  # without needing a type check here.
  def self.assert_empty(collection, message: String = "") -> Bool
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(collection.length() == 0,
      "#{prefix}expected #{JSON.stringify(collection)} to be empty")
  end

  def self.refute_empty(collection, message: String = "") -> Bool
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(collection.length() != 0,
      "#{prefix}expected #{JSON.stringify(collection)} not to be empty")
  end

  # For Float comparisons, where exact equality is usually the wrong
  # question to ask. `delta` defaults to real minitest's own default
  # (0.001). Written without `.abs()` -- Int/Float values have no method
  # dispatch of their own in Diamond (see tests/run.sh's own "Int
  # literal .abs() unexpectedly succeeded" check) -- a plain conditional
  # negation instead.
  def self.assert_in_delta(expected, actual, delta = 0.001, message: String = "") -> Bool
    difference = expected - actual
    if difference < 0
      difference = -difference
    end
    prefix = if message == "" then "" else message + "\n" end
    Minitest.assert(difference <= delta,
      "#{prefix}expected #{JSON.stringify(actual)} to be within #{JSON.stringify(delta)} of #{JSON.stringify(expected)}")
  end

  # Runs `action` and asserts it raised an exception matching E (or a
  # subclass of it). E must be given explicitly at the call site, e.g.
  # `Minitest.assert_raises[SomeError](action)` -- there's no argument
  # E could be inferred from, and no default type argument. Use
  # `assert_raises[StandardError](action)` for "just confirm something
  # was raised, any kind". An action that raises some *other*,
  # unrelated exception type isn't caught here at all -- it propagates
  # past assert_raises the same way an unmatched `rescue error: E`
  # always does, surfacing as an error rather than a clean failure.
  def self.assert_raises[E](action: Callable[0], message: String = "expected an exception to be raised") -> Bool
    raised = begin
      action()
      false
    rescue error: AssertionError
      raise
    rescue error: E
      true
    end
    if raised
      true
    else
      raise AssertionError.new(message)
    end
  end

  # Marks the currently running test skipped rather than passed, failed,
  # or errored -- call from inside a registered test callback, e.g. to
  # bail out of a test that depends on something not available in the
  # current environment. Unlike a failed assertion, a skip doesn't make
  # `run`/`run!` report the suite as unsuccessful.
  def self.skip(reason: String = "skipped")
    raise Skip.new(reason)
  end

  def test(name: String, callback: Callable[0]) -> Minitest
    @names.push(name)
    @callbacks.push(callback)
    self
  end

  # Runs before every registered test, prior to the test's own callback.
  # Calling this again replaces the previously registered hook rather
  # than adding a second one.
  def setup(callback: Callable[0]) -> Minitest
    @setup_hook = callback
    self
  end

  # Runs after every registered test, via `ensure` -- so it still runs
  # even when the test failed, errored, or called Minitest.skip. Calling
  # this again replaces the previously registered hook.
  def teardown(callback: Callable[0]) -> Minitest
    @teardown_hook = callback
    self
  end

  def total() -> Int
    @names.length()
  end

  # Runs every registered test, prints a summary, and returns true only
  # if every test passed (a skip doesn't count against this).
  def run() -> Bool
    index = 0
    while index < @names.length()
      name = @names[index]
      callback = @callbacks[index]
      setup_hook = @setup_hook
      teardown_hook = @teardown_hook
      begin
        unless setup_hook == nil
          setup_hook()
        end
        callback()
        @passed = @passed + 1
      rescue error: Skip
        @skipped = @skipped + 1
        @failures.push("SKIP: #{name}\n  #{error.message()}")
      rescue error: AssertionError
        @failed = @failed + 1
        @failures.push("FAIL: #{name}\n  #{error.message()}")
      rescue error: StandardError
        @errored = @errored + 1
        @failures.push("ERROR: #{name} (#{error.class()})\n  #{error.message()}")
      ensure
        unless teardown_hook == nil
          teardown_hook()
        end
      end
      index += 1
    end
    self.report()
    @failed == 0 && @errored == 0
  end

  def report()
    index = 0
    while index < @failures.length()
      puts(@failures[index])
      index += 1
    end
    puts("#{@names.length()} tests, #{@passed} passed, #{@failed} failed, #{@errored} errors, #{@skipped} skipped")
  end

  # Same as `run`, but raises if anything failed or errored -- lets a
  # test file's own exit code (via the `diamond` CLI's uncaught-
  # exception handling) reflect pass/fail, e.g. for CI.
  def run!() -> Bool
    if self.run()
      true
    else
      raise RuntimeError.new("#{@failed + @errored} of #{@names.length()} test(s) did not pass")
    end
  end
end
