# A `module_function`-mode method's exported (qualified-call-reachable)
# singleton descriptor used to only get registered *after* its own body
# finished compiling -- so a self-referencing qualified call inside
# that same body (`ModuleName.method(...)` calling itself) could never
# find its own not-yet-registered descriptor: "undefined module
# singleton function" at compile time. A previously-undiscovered gap,
# not a documented cut. Found building packages/graphql (worked around
# there by using a `self.`-method class instead of `module_function`
# for anything that needed to recurse). Fixed by registering the
# descriptor as soon as the parameter list (and therefore name/arity)
# is known, before the body compiles, instead of after.
#
# Still NOT supported, a separate and harder problem this fix doesn't
# attempt (see docs/roadmap.md or ask before assuming it's fixed):
# mutual recursion between two *different* module_function siblings
# where the callee is defined later in the same module -- that would
# need every module_function method's name discovered before any of
# their bodies compile, not just each one's own name before its own
# body.
require "../../lib/minitest"

module Factorial
  module_function
  def compute(n)
    if n <= 1
      1
    else
      n * Factorial.compute(n - 1)
    end
  end
end

module Fibonacci
  module_function
  def at(n)
    if n <= 1
      n
    else
      Fibonacci.at(n - 1) + Fibonacci.at(n - 2)
    end
  end
end

# A sibling call to an *earlier*-defined module_function method already
# worked before this fix (the earlier method's descriptor is registered
# -- old way or new way, doesn't matter which -- by the time the later
# one's body compiles) -- confirming this fix didn't regress that case.
module Utils
  module_function
  def double(n) = n * 2
  def quadruple(n) = Utils.double(Utils.double(n))
end

def run_tests()
  def test_self_recursive_qualified_call()
    Minitest.assert_equal(120, Factorial.compute(5))
  end

  def test_self_recursive_qualified_call_two_base_cases()
    Minitest.assert_equal(55, Fibonacci.at(10))
  end

  def test_sibling_call_to_earlier_defined_method_still_works()
    Minitest.assert_equal(20, Utils.quadruple(5))
  end

  suite = Minitest.new()
  suite.test("module_function method calling itself via a qualified call",
             test_self_recursive_qualified_call)
  suite.test("module_function method recursing with two base cases",
             test_self_recursive_qualified_call_two_base_cases)
  suite.test("sibling call to an earlier-defined module_function method",
             test_sibling_call_to_earlier_defined_method_still_works)
  suite.run!()
end

run_tests()
