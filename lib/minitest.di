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
# Diamond has no runtime reflection -- no dynamic dispatch by string
# name, no first-class Class values (see docs/roadmap.md) -- so tests
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
# other StandardError (recorded as an error), and anything else
# (no exception) as a pass.

class AssertionError < StandardError
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
    @failures = []
  end

  attr_reader passed: Int
  attr_reader failed: Int
  attr_reader errored: Int
  attr_reader failures: Array

  def self.assert(condition: Bool, message: String = "assertion failed") -> Bool
    if !condition
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

  def test(name: String, callback: Callable[0]) -> Minitest
    @names.push(name)
    @callbacks.push(callback)
    self
  end

  def total() -> Int
    @names.length()
  end

  # Runs every registered test, prints a summary, and returns true only
  # if every test passed.
  def run() -> Bool
    index = 0
    while index < @names.length()
      name = @names[index]
      callback = @callbacks[index]
      begin
        callback()
        @passed = @passed + 1
      rescue error: AssertionError
        @failed = @failed + 1
        @failures.push("FAIL: #{name}\n  #{error.message()}")
      rescue error: StandardError
        @errored = @errored + 1
        @failures.push("ERROR: #{name}\n  #{error.message()}")
      end
      index = index + 1
    end
    self.report()
    @failed == 0 && @errored == 0
  end

  def report()
    index = 0
    while index < @failures.length()
      puts(@failures[index])
      index = index + 1
    end
    puts("#{@names.length()} tests, #{@passed} passed, #{@failed} failed, #{@errored} errors")
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
