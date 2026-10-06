# Array#push and #length (and Hash/String#length) are answered at the
# INVOKE_TYPED call site, without the call into collection_invoke_helper
# (collection_invoke_fast in src/vm.c). The fast path must give the same
# results as the helper for the cases it takes and leave every other case --
# an arity mismatch, a frozen array, a type-constrained array -- to the
# helper's own errors.
require "../../lib/minitest"

def run_tests()
  def test_push_and_length_in_a_loop()
    values = []
    index = 0
    while index < 50
      values.push(index)
      index = index + 1
    end
    Minitest.assert_equal(50, values.length())
    Minitest.assert_equal(49, values[49])
  end

  def test_push_returns_the_receiver()
    values = [1]
    Minitest.assert_equal([1, 2], values.push(2))
  end

  def test_length_on_each_collection_kind()
    Minitest.assert_equal(3, [1, 2, 3].length())
    pairs = {"a": 1, "b": 2}
    Minitest.assert_equal(2, pairs.length())
    Minitest.assert_equal(5, "hello".length())
  end

  def push_to_a_frozen_array()
    [1].freeze.push(2)
  end

  def test_push_to_a_frozen_array_still_fails()
    Minitest.assert_raises[StandardError](push_to_a_frozen_array)
  end

  def push_two_arguments()
    [1].push(2, 3)
  end

  def test_push_arity_is_still_checked()
    Minitest.assert_raises[StandardError](push_two_arguments)
  end

  def length_with_an_argument()
    [1].length(1)
  end

  def test_length_arity_is_still_checked()
    Minitest.assert_raises[StandardError](length_with_an_argument)
  end

  def push_a_string_to_ints(values: Array[Int])
    values.push("x")
  end

  def push_an_int_to_ints(values: Array[Int])
    values.push(2)
    values.length()
  end

  def push_string_into_typed()
    push_a_string_to_ints([1])
  end

  def test_typed_array_constraint_is_still_checked()
    Minitest.assert_equal(2, push_an_int_to_ints([1]))
    Minitest.assert_raises[StandardError](push_string_into_typed)
  end

  suite = Minitest.new()
  suite.test("push and length in a loop", test_push_and_length_in_a_loop)
  suite.test("push returns the receiver", test_push_returns_the_receiver)
  suite.test("length on each collection kind", test_length_on_each_collection_kind)
  suite.test("push to a frozen array fails", test_push_to_a_frozen_array_still_fails)
  suite.test("push arity", test_push_arity_is_still_checked)
  suite.test("length arity", test_length_arity_is_still_checked)
  suite.test("typed array", test_typed_array_constraint_is_still_checked)
  suite.run!()
end

run_tests()
