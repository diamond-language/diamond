require "../../lib/minitest"

def run_tests()
  def test_pass()
    Minitest.assert_equal(4, 2 + 2)
  end

  def test_wrong_value()
    Minitest.assert_equal(5, 2 + 2)
  end

  def test_division_by_zero()
    1 / 0
  end

  def raises_runtime_error()
    raise RuntimeError.new("boom")
  end

  def test_raises_caught()
    Minitest.assert_raises[RuntimeError](raises_runtime_error)
  end

  def test_raises_wrong_type()
    Minitest.assert_raises[ArgumentError](raises_runtime_error)
  end

  def does_not_raise()
    1 + 1
  end

  def test_raises_not_caught()
    Minitest.assert_raises[RuntimeError](does_not_raise)
  end

  suite = Minitest.new()
  suite.test("passes", test_pass)
  suite.test("wrong value", test_wrong_value)
  suite.test("division by zero", test_division_by_zero)
  suite.test("raises caught", test_raises_caught)
  suite.test("raises wrong type", test_raises_wrong_type)
  suite.test("raises not caught", test_raises_not_caught)
  suite.run()
end

run_tests()
