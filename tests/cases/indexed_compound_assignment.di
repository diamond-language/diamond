# Indexed compound assignment -- `arr[i] += 1`, `h[k] -= 1`, etc. See
# docs/roadmap.md and docs/syntax.md. Compiles into an INDEX_GET/INDEX_SET
# pair around the existing operator-dispatch shape ordinary compound
# assignment (`x += 1`) already has -- this suite's real point is the two
# things that could easily go subtly wrong: the index expression must be
# evaluated exactly once (not once for the read and again for the write),
# and every receiver shape ordinary indexed assignment already supports
# (local, @ivar, @@cvar, a captured/boxed local) must keep working the
# same way for the compound form.
require "../../lib/minitest"

class Box
  def initialize()
    @items = [1, 2, 3]
  end
  def bump(i)
    @items[i] += 100
  end
  def items()
    @items
  end
end

class Counter
  def self.reset()
    @@totals = [0, 0]
  end
  def self.bump(i)
    @@totals[i] += 1
  end
  def self.totals()
    @@totals
  end
end

def make_bumper()
  data = [1, 2, 3]
  def bump_captured(i)
    data[i] += 1000
    data
  end
  bump_captured
end

def run_tests()
  def test_all_seven_operators_on_an_array()
    arr = [1, 2, 3]
    arr[0] += 10
    arr[1] -= 1
    arr[2] *= 3
    Minitest.assert_equal(11, arr[0])
    Minitest.assert_equal(1, arr[1])
    Minitest.assert_equal(9, arr[2])

    nums = [10, 20]
    nums[0] /= 2
    nums[1] %= 3
    Minitest.assert_equal(5, nums[0])
    Minitest.assert_equal(2, nums[1])

    flags = [nil, true]
    flags[0] ||= "default"
    flags[1] &&= "changed"
    Minitest.assert_equal("default", flags[0])
    Minitest.assert_equal("changed", flags[1])
  end

  def test_compound_assignment_on_a_hash_value()
    h = {"a": 1}
    h["a"] += 5
    Minitest.assert_equal(6, h["a"])
  end

  def test_index_expression_evaluated_exactly_once()
    call_count = [0]
    def next_index(calls)
      calls[0] += 1
      1
    end
    arr = [10, 20, 30]
    arr[next_index(call_count)] += 5
    Minitest.assert_equal(10, arr[0])
    Minitest.assert_equal(25, arr[1])
    Minitest.assert_equal(30, arr[2])
    Minitest.assert_equal(1, call_count[0])
  end

  def test_ivar_receiver()
    b = Box.new()
    b.bump(1)
    items = b.items()
    Minitest.assert_equal(1, items[0])
    Minitest.assert_equal(102, items[1])
    Minitest.assert_equal(3, items[2])
  end

  def test_cvar_receiver()
    Counter.reset()
    Counter.bump(0)
    Counter.bump(0)
    Counter.bump(1)
    totals = Counter.totals()
    Minitest.assert_equal(2, totals[0])
    Minitest.assert_equal(1, totals[1])
  end

  def test_captured_local_receiver()
    bump_fn = make_bumper()
    result = bump_fn(0)
    Minitest.assert_equal(1001, result[0])
    Minitest.assert_equal(2, result[1])
    Minitest.assert_equal(3, result[2])
  end

  def test_plain_indexed_assignment_still_works()
    arr = [1, 2, 3]
    arr[0] = 99
    Minitest.assert_equal(99, arr[0])
    Minitest.assert_equal(2, arr[1])
    Minitest.assert_equal(3, arr[2])
  end

  def test_plain_compound_assignment_still_works()
    x = 1
    x += 5
    Minitest.assert_equal(6, x)
  end

  suite = Minitest.new()
  suite.test("all seven operators on an array") do
    test_all_seven_operators_on_an_array()
  end
  suite.test("compound assignment on a hash value") do
    test_compound_assignment_on_a_hash_value()
  end
  suite.test("index expression evaluated exactly once") do
    test_index_expression_evaluated_exactly_once()
  end
  suite.test("@ivar receiver") do
    test_ivar_receiver()
  end
  suite.test("@@cvar receiver") do
    test_cvar_receiver()
  end
  suite.test("captured (boxed) local receiver") do
    test_captured_local_receiver()
  end
  suite.test("plain indexed assignment still works") do
    test_plain_indexed_assignment_still_works()
  end
  suite.test("plain compound assignment still works") do
    test_plain_compound_assignment_still_works()
  end
  suite.run!()
end

run_tests()
