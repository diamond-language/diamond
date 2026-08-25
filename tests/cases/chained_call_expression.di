# `expr(...)` directly after an already-fully-parsed expression, with
# no `.method`/`[index]` in between -- calling the Callable value
# `expr` itself, e.g. `type.coerce_input()(value)`, `(a)(b)`,
# `arr[0]()`. parse_precedence's own postfix-chaining loop only ever
# checked for `.`/`[` after an expression, never a bare `(` -- so a
# call directly on a call's own result (no method name, no local
# variable to bind it to first) fell through to ordinary expression
# parsing, which happily finished at the first `)`, leaving the second
# `(...)` as unconsumed tokens -- "expected newline after expression".
# A previously-undiscovered gap, not a documented cut. Found building
# packages/graphql, which needed exactly `type.coerce_input()(value)`
# and worked around it with an intermediate local
# (`coercer = type.coerce_input(); coercer(value)`) until this fixed
# it directly. parse_closure_call_arguments already existed for
# "call whatever Callable is in this register" (shared with a local
# variable or `@ivar`/`@@cvar` holding one, both followed by `(...)`)
# -- this just extends where that register is allowed to come from.
require "../../lib/minitest"

class Box
  def make_doubler()
    def inner(x) = x * 2
    inner
  end
end

def make_adder(n)
  def add(x) = x + n
  add
end

def choose(flag)
  def a(x) = x + 1
  def b(x) = x + 2
  if flag then a else b end
end

def run_tests()
  def test_call_directly_on_method_calls_own_result()
    box = Box.new()
    Minitest.assert_equal(42, box.make_doubler()(21))
  end

  def test_call_directly_on_top_level_calls_own_result()
    Minitest.assert_equal(15, make_adder(5)(10))
  end

  def test_ordinary_local_variable_call_still_works()
    f = make_adder(1)
    Minitest.assert_equal(3, f(2))
  end

  def test_call_on_an_if_expressions_own_result()
    Minitest.assert_equal(11, choose(true)(10))
    Minitest.assert_equal(12, choose(false)(10))
  end

  def test_call_on_a_parenthesized_expressions_own_result()
    Minitest.assert_equal(15, (make_adder(5))(10))
  end

  def test_call_after_array_index()
    callables = [make_adder(100)]
    Minitest.assert_equal(101, callables[0](1))
  end

  suite = Minitest.new()
  suite.test("call directly on a method call's own result", test_call_directly_on_method_calls_own_result)
  suite.test("call directly on a top-level call's own result", test_call_directly_on_top_level_calls_own_result)
  suite.test("ordinary local-variable call still works", test_ordinary_local_variable_call_still_works)
  suite.test("call on an if-expression's own result", test_call_on_an_if_expressions_own_result)
  suite.test("call on a parenthesized expression's own result", test_call_on_a_parenthesized_expressions_own_result)
  suite.test("call after array index", test_call_after_array_index)
  suite.run!()
end

run_tests()
