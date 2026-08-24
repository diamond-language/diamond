# `foo(*array)` -- call-site spread, the caller-side counterpart to
# `def foo(*rest)` (tests/cases/variadic_parameters.di). Only a bare
# top-level function call, and only when the spread argument is the
# call's sole argument -- see docs/design.md's "Call-site spread"
# section for the full scope and mechanism.
require "../../lib/minitest"

def sum3(a, b, c)
  a + b + c
end

def sum(*nums)
  total = 0
  nums.each() do |n| total += n end
  total
end

def zero_arg()
  "called"
end

def describe(a, *rest)
  "a=#{a} rest=#{rest}"
end

def run_tests()
  def test_spreads_exactly_matching_a_fixed_arity_function()
    Minitest.assert_equal(6, sum3(*[1, 2, 3]))
  end

  def test_spreads_into_a_variadic_function()
    Minitest.assert_equal("a=10 rest=[20, 30, 40]", describe(*[10, 20, 30, 40]))
  end

  def test_spreads_an_empty_array_into_a_zero_arg_function()
    Minitest.assert_equal("called", zero_arg(*[]))
  end

  def test_spread_bypasses_the_16_argument_expression_cap()
    # A literal call site is capped at 16 argument expressions
    # (independent of this feature -- see variadic_parameters.di's own
    # comment), but a spread array has no such cap: its length is a
    # runtime value, not one argument expression per element.
    big = (1..50).to_a()
    Minitest.assert_equal(1275, sum(*big))
  end

  def test_spread_rejects_a_non_array()
    def spread_a_string()
      sum3(*"not an array")
    end
    Minitest.assert_raises[TypeError](spread_a_string)
  end

  def test_spread_rejects_too_few_elements()
    def spread_too_few()
      sum3(*[1, 2])
    end
    Minitest.assert_raises[ArgumentError](spread_too_few)
  end

  def test_spread_rejects_too_many_for_non_variadic()
    def spread_too_many()
      sum3(*[1, 2, 3, 4])
    end
    Minitest.assert_raises[ArgumentError](spread_too_many)
  end

  suite = Minitest.new()
  suite.test("spreads exactly matching a fixed-arity function",
             test_spreads_exactly_matching_a_fixed_arity_function)
  suite.test("spreads into a variadic function", test_spreads_into_a_variadic_function)
  suite.test("spreads an empty array into a zero-arg function",
             test_spreads_an_empty_array_into_a_zero_arg_function)
  suite.test("spread bypasses the 16-argument-expression cap",
             test_spread_bypasses_the_16_argument_expression_cap)
  suite.test("spread rejects a non-Array", test_spread_rejects_a_non_array)
  suite.test("spread rejects too few elements", test_spread_rejects_too_few_elements)
  suite.test("spread rejects too many elements for a non-variadic function",
             test_spread_rejects_too_many_for_non_variadic)
  suite.run!()
end

run_tests()
