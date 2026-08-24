# `def foo(bar, baz, *other)` -- a trailing variadic parameter collects
# every argument beyond the fixed (non-variadic) ones into an ordinary
# Array. See docs/design.md's "Splat/variadic parameters" section.
#
# The compile-time-rejected shapes (a variadic parameter that isn't last,
# more than one variadic parameter, a type annotation or default value on
# one, and a statically-known-callee call site with too few arguments)
# were verified directly against the built CLI rather than encoded here
# -- same reasoning tests/cases/method_delegation.di's own header already
# gives: a compile error doesn't fit this file's two available shapes
# (a stdout diff or this file's own Minitest exit-code suite).
require "../../lib/minitest"

def sum(*nums)
  total = 0
  nums.each() do |n| total += n end
  total
end

def two_required(a, b, *rest)
  a + b + rest.length()
end

# "16 args at one call site" is the real ceiling here, not an arbitrary
# round number -- every call-argument-parsing path in the compiler caps
# at 16 argument expressions per call site regardless of the callee
# (confirmed directly: a 17th argument fails to compile with "too many
# call arguments" even against this variadic sum), independent of
# variadic support. This still meaningfully exercises run_chunk's own
# bounds fix (src/vm.c): sum's own register_count is far smaller than 16.
def sixteen_args_total()
  sum(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16)
end

class Greeter
  def initialize(prefix)
    @prefix = prefix
  end
  def greet(name, *titles)
    "#{@prefix} #{titles.join(" ")} #{name}".strip()
  end
  def self.announce(name, *titles)
    "announcing: #{titles.join(" ")} #{name}".strip()
  end
  # A non-variadic replacement for a variadic original -- required_arity
  # (2 vs 1) and has_variadic (false vs true) both mismatch, so this
  # should always be rejected.
  def self.mismatched_bar_factory()
    def mismatched_bar(a, b)
      "mismatched a=#{a} b=#{b}"
    end
    mismatched_bar
  end
  # Same shape as bar itself (arity 2, required_arity 1, variadic) --
  # should be accepted.
  def self.matching_bar_factory()
    def matching_bar(a, *rest)
      "matched a=#{a} rest=#{rest.length()}"
    end
    matching_bar
  end
  def bar(a, *rest)
    "orig a=#{a} rest=#{rest.length()}"
  end
  # closure requires an enclosing method (a real `self`) -- see
  # docs/design.md's `closure name() ... end` section -- so this is
  # exercised from inside an instance method, not at top level.
  def closure_variadic(a, extra1, extra2)
    closure inner(a, *rest)
      "#{a}:#{rest.length()}"
    end
    inner(a, extra1, extra2)
  end
end

def apply_callable2(callback: Callable[2], a, b)
  callback(a, b)
end

def apply_callable5(callback: Callable[5], a, b, c, d, e)
  callback(a, b, c, d, e)
end

def run_tests()
  def test_zero_trailing_args_collect_an_empty_array()
    Minitest.assert_equal(0, sum())
  end

  def test_a_few_trailing_args_collect_correctly()
    Minitest.assert_equal(6, sum(1, 2, 3))
  end

  def test_sixteen_args_at_one_call_site()
    Minitest.assert_equal(136, sixteen_args_total())
  end

  def test_fixed_parameters_still_bind_normally()
    Minitest.assert_equal("hi Dr Prof Ada", Greeter.new("hi").greet("Ada", "Dr", "Prof"))
  end

  def test_instance_method_variadic()
    # Two spaces, not one: titles.join(" ") on an empty Array is "",
    # still contributing its own surrounding space in the template.
    Minitest.assert_equal("hi  Ada", Greeter.new("hi").greet("Ada"))
  end

  def test_self_singleton_method_variadic()
    Minitest.assert_equal("announcing: Captain Ahab", Greeter.announce("Ahab", "Captain"))
  end

  def test_closure_with_declared_parameters_variadic()
    Minitest.assert_equal("x:2", Greeter.new("").closure_variadic("x", 1, 2))
  end

  def test_callable_n_matches_a_variadic_closure_at_required_arity()
    Minitest.assert_equal(3, apply_callable2(two_required, 1, 2))
  end

  def test_callable_n_matches_a_variadic_closure_above_required_arity()
    Minitest.assert_equal(15, apply_callable5(sum, 1, 2, 3, 4, 5))
  end

  def test_redefine_method_accepts_a_matching_variadic_replacement()
    g = Greeter.new("")
    Minitest.assert_equal("orig a=1 rest=1", g.bar(1, 2))
    Greeter.redefine_method("bar", Greeter.matching_bar_factory())
    Minitest.assert_equal("matched a=1 rest=1", g.bar(1, 2))
  end

  def test_redefine_method_rejects_variadic_mismatch()
    def mismatched_redefine()
      Greeter.redefine_method("bar", Greeter.mismatched_bar_factory())
    end
    Minitest.assert_raises[ArgumentError](mismatched_redefine)
  end

  suite = Minitest.new()
  suite.test("zero trailing args collect an empty array",
             test_zero_trailing_args_collect_an_empty_array)
  suite.test("a few trailing args collect correctly", test_a_few_trailing_args_collect_correctly)
  suite.test("sixteen args at one call site", test_sixteen_args_at_one_call_site)
  suite.test("fixed parameters still bind normally", test_fixed_parameters_still_bind_normally)
  suite.test("instance method variadic", test_instance_method_variadic)
  suite.test("self. singleton method variadic", test_self_singleton_method_variadic)
  suite.test("closure with declared parameters variadic",
             test_closure_with_declared_parameters_variadic)
  suite.test("Callable[N] matches a variadic closure at required_arity",
             test_callable_n_matches_a_variadic_closure_at_required_arity)
  suite.test("Callable[N] matches a variadic closure above required_arity",
             test_callable_n_matches_a_variadic_closure_above_required_arity)
  suite.test("redefine_method accepts a matching variadic replacement",
             test_redefine_method_accepts_a_matching_variadic_replacement)
  suite.test("redefine_method rejects a variadic vs non-variadic mismatch",
             test_redefine_method_rejects_variadic_mismatch)
  suite.run!()
end

run_tests()
