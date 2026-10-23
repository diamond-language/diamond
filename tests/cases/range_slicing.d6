# Range-based Array slicing -- `arr[1..3]` (read) and `arr[1..3] = [...]`
# (write, requiring an exact-length replacement -- no Ruby-style grow/shrink
# splice in this first version). See docs/roadmap.md and docs/syntax.md.
# Array only -- Hash's own `[]` is plain key lookup, in Diamond as in real
# Ruby, so it's untouched by this and covered here only to confirm that.
require "../../lib/minitest"

def run_tests()
  def test_inclusive_and_exclusive_reads()
    arr = [10, 20, 30, 40, 50]
    inclusive = arr[1..3]
    Minitest.assert_equal(3, inclusive.length())
    Minitest.assert_equal(20, inclusive[0])
    Minitest.assert_equal(40, inclusive[2])

    exclusive = arr[1...3]
    Minitest.assert_equal(2, exclusive.length())
    Minitest.assert_equal(20, exclusive[0])
    Minitest.assert_equal(30, exclusive[1])
  end

  def test_out_of_range_end_is_clamped_not_an_error()
    arr = [10, 20, 30, 40, 50]
    sliced = arr[3..100]
    Minitest.assert_equal(2, sliced.length())
    Minitest.assert_equal(40, sliced[0])
    Minitest.assert_equal(50, sliced[1])
  end

  def test_empty_and_reversed_ranges_yield_empty_array()
    arr = [10, 20, 30]
    Minitest.assert_equal(0, arr[3...3].length())
    Minitest.assert_equal(0, arr[2..0].length())
    # start exactly at the array's own length is a valid, empty slice --
    # not an out-of-bounds error (matches String#slice's own convention).
    Minitest.assert_equal(0, arr[3..5].length())
  end

  def test_start_out_of_bounds_raises_index_error()
    arr = [1, 2, 3]
    message = nil
    begin
      arr[10..20]
    rescue error: IndexError
      message = error.message()
    end
    Minitest.assert_equal("index 10 out of bounds for Array of length 3", message)
  end

  def test_slice_write_with_matching_length()
    arr = [1, 2, 3, 4, 5]
    arr[1..3] = [200, 300, 400]
    Minitest.assert_equal(5, arr.length())
    Minitest.assert_equal(1, arr[0])
    Minitest.assert_equal(200, arr[1])
    Minitest.assert_equal(300, arr[2])
    Minitest.assert_equal(400, arr[3])
    Minitest.assert_equal(5, arr[4])
  end

  def test_mismatched_length_write_raises_untouched()
    arr = [1, 2, 3, 4, 5]
    message = nil
    begin
      arr[1..3] = [999]
    rescue error: TypeError
      message = error.message()
    end
    Minitest.assert_equal(
      "range assignment requires a replacement Array of exactly 3 element(s)", message)
    Minitest.assert_equal(1, arr[0])
    Minitest.assert_equal(2, arr[1])
    Minitest.assert_equal(3, arr[2])
    Minitest.assert_equal(4, arr[3])
    Minitest.assert_equal(5, arr[4])
  end

  def test_plain_int_indexing_is_unaffected()
    arr = [1, 2, 3]
    Minitest.assert_equal(2, arr[1])
    arr[1] = 99
    Minitest.assert_equal(99, arr[1])
  end

  def test_hash_bracket_lookup_is_unaffected()
    h = {"a": 1, "b": 2}
    Minitest.assert_equal(1, h["a"])
    h["a"] = 99
    Minitest.assert_equal(99, h["a"])
  end

  suite = Minitest.new()
  suite.test("inclusive and exclusive range reads") do
    test_inclusive_and_exclusive_reads()
  end
  suite.test("out-of-range end is clamped, not an error") do
    test_out_of_range_end_is_clamped_not_an_error()
  end
  suite.test("empty and reversed ranges yield an empty array") do
    test_empty_and_reversed_ranges_yield_empty_array()
  end
  suite.test("start out of bounds raises IndexError") do
    test_start_out_of_bounds_raises_index_error()
  end
  suite.test("slice write with matching length") do
    test_slice_write_with_matching_length()
  end
  suite.test("slice write with mismatched length raises, array untouched") do
    test_mismatched_length_write_raises_untouched()
  end
  suite.test("plain Int indexing is unaffected") do
    test_plain_int_indexing_is_unaffected()
  end
  suite.test("Hash bracket lookup is unaffected") do
    test_hash_bracket_lookup_is_unaffected()
  end
  suite.run!()
end

run_tests()
