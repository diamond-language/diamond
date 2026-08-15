require "../../lib/minitest"

def run_tests()
  def test_refute_nil_fail()
    Minitest.refute_nil(nil)
  end

  def test_assert_includes_fail()
    Minitest.assert_includes([1, 2, 3], 9)
  end

  def test_refute_includes_fail()
    Minitest.refute_includes([1, 2, 3], 2)
  end

  def test_assert_empty_fail()
    Minitest.assert_empty([1])
  end

  def test_refute_empty_fail()
    Minitest.refute_empty([])
  end

  def test_assert_in_delta_fail()
    Minitest.assert_in_delta(1.0, 2.0)
  end

  suite = Minitest.new()
  suite.test("refute_nil fail", test_refute_nil_fail)
  suite.test("assert_includes fail", test_assert_includes_fail)
  suite.test("refute_includes fail", test_refute_includes_fail)
  suite.test("assert_empty fail", test_assert_empty_fail)
  suite.test("refute_empty fail", test_refute_empty_fail)
  suite.test("assert_in_delta fail", test_assert_in_delta_fail)
  suite.run()
end

run_tests()
