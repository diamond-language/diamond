require "../../lib/minitest"

def run_tests()
  def test_fails()
    Minitest.assert_equal(1, 2)
  end

  suite = Minitest.new()
  suite.test("fails", test_fails)
  suite.run!()
end

run_tests()
