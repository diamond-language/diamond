require "../../lib/minitest"

def run_tests()
  def make_log()
    []
  end

  log = make_log()

  def make_setup(log)
    def setup_hook()
      log.push("setup")
    end
    setup_hook
  end

  def make_teardown(log)
    def teardown_hook()
      log.push("teardown")
    end
    teardown_hook
  end

  def test_normal(log)
    def inner()
      log.push("test")
    end
    inner
  end

  def test_skipped()
    Minitest.skip("not ready yet")
  end

  def test_fails_but_teardown_still_runs()
    Minitest.assert_equal(1, 2)
  end

  suite = Minitest.new()
  suite.setup(make_setup(log))
  suite.teardown(make_teardown(log))
  suite.test("normal", test_normal(log))
  suite.test("skipped", test_skipped)
  suite.test("fails", test_fails_but_teardown_still_runs)
  suite.run()

  "#{log.length()}|#{suite.passed()}|#{suite.failed()}|#{suite.errored()}|#{suite.skipped()}"
end

run_tests()
