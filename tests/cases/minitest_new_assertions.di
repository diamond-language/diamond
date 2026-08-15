require "../../lib/minitest"

def run_tests()
  def test_refute_nil()
    Minitest.refute_nil(5)
  end

  def test_assert_includes()
    Minitest.assert_includes([1, 2, 3], 2)
  end

  def test_refute_includes()
    Minitest.refute_includes([1, 2, 3], 4)
  end

  def test_assert_empty_array()
    Minitest.assert_empty([])
  end

  def test_refute_empty_array()
    Minitest.refute_empty([1])
  end

  def test_assert_empty_string()
    Minitest.assert_empty("")
  end

  def test_assert_in_delta()
    Minitest.assert_in_delta(1.0, 1.0005)
  end

  suite = Minitest.new()
  suite.test("refute_nil", test_refute_nil)
  suite.test("assert_includes", test_assert_includes)
  suite.test("refute_includes", test_refute_includes)
  suite.test("assert_empty array", test_assert_empty_array)
  suite.test("refute_empty array", test_refute_empty_array)
  suite.test("assert_empty string", test_assert_empty_string)
  suite.test("assert_in_delta", test_assert_in_delta)
  suite.run!()
end

run_tests()
