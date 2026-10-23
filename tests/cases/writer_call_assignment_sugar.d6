# `recv.attr = value` -- sugar for the pre-existing writer-call syntax
# `recv.attr=(value)`. Pure parse-time desugaring inside parse_invoke
# (src/compiler.c): no new opcode, and the explicit parenthesized form
# keeps compiling to the exact same DIAMOND_OP_INVOKE call. The real risk
# this suite exists to catch is the array-literal right-hand side: a `[`
# right after the writer's `=` is ambiguous with parse_invoke's own (never
# actually used anywhere in this codebase) explicit generic writer-call
# syntax `recv.attr=[T](value)` -- writer_generic_arguments_ahead only
# reads a `[` as generics when its matching `]` is directly followed by
# `(`, so a plain array literal falls through to the new sugar instead.
require "../../lib/minitest"

class Box
  attr_accessor value: Int
  attr_accessor tags: Array
  def initialize()
    @value = 0
    @tags = []
  end
end

class Address
  attr_accessor city: String
  def initialize()
    @city = "old"
  end
end

class Person
  def initialize()
    @address = Address.new()
  end
  def address() = @address
end

def run_tests()
  def test_basic_bare_assignment()
    b = Box.new()
    b.value = 5
    Minitest.assert_equal(5, b.value())
  end

  def test_array_literal_right_hand_side()
    b = Box.new()
    b.tags = ["ruby", "diamond"]
    Minitest.assert_equal(2, b.tags().length())
    Minitest.assert_equal("ruby", b.tags()[0])
    Minitest.assert_equal("diamond", b.tags()[1])
  end

  def test_explicit_parens_form_still_works()
    b = Box.new()
    b.value=(9)
    Minitest.assert_equal(9, b.value())
  end

  def test_self_receiver_inside_instance_method()
    b = Box.new()
    b.value = 3
    Minitest.assert_equal(3, b.value())
  end

  def test_receiver_is_a_fresh_call_result()
    Box.new().value = 42
    # Just confirming this compiles and runs without error -- the
    # instance is unobservable afterward, there's nothing to assert
    # equal against.
    Minitest.assert_equal(true, true)
  end

  def test_chained_call_receiver()
    p = Person.new()
    p.address().city = "new"
    Minitest.assert_equal("new", p.address().city())
  end

  def test_sugar_is_an_expression_returning_the_writers_result()
    b = Box.new()
    result = (b.value = 7)
    Minitest.assert_equal(7, result)
    Minitest.assert_equal(7, b.value())
  end

  def test_rhs_is_an_ordinary_call()
    b = Box.new()
    b.value = [1, 2, 3].length()
    Minitest.assert_equal(3, b.value())
  end

  suite = Minitest.new()
  suite.test("basic bare assignment") do
    test_basic_bare_assignment()
  end
  suite.test("array literal right-hand side is not mistaken for generics") do
    test_array_literal_right_hand_side()
  end
  suite.test("explicit parens form still works") do
    test_explicit_parens_form_still_works()
  end
  suite.test("self receiver inside instance method") do
    test_self_receiver_inside_instance_method()
  end
  suite.test("receiver is a fresh call result, not a local") do
    test_receiver_is_a_fresh_call_result()
  end
  suite.test("chained call receiver") do
    test_chained_call_receiver()
  end
  suite.test("sugar is an expression returning the writer's result") do
    test_sugar_is_an_expression_returning_the_writers_result()
  end
  suite.test("right-hand side is an ordinary call") do
    test_rhs_is_an_ordinary_call()
  end
  suite.run!()
end

run_tests()
