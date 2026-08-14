require "../../lib/minitest"

def run_tests()
  def test_arithmetic()
    Minitest.assert_equal(4, 2 + 2)
  end

  def test_boolean()
    Minitest.assert(true)
    Minitest.refute(false)
  end

  def test_nil()
    Minitest.assert_nil(nil)
  end

  suite = Minitest.new()
  suite.test("arithmetic", test_arithmetic)
  suite.test("boolean checks", test_boolean)
  suite.test("nil check", test_nil)
  suite.run!()
end

run_tests()
