# `x[a][b]... = value` (more than one `[...]` group before the `=`)
# previously only recognized a *single* index group before checking for
# `=` -- index_assignment_ahead scanned exactly one balanced `[...]`
# group then required `=` immediately after, so `x[a][b] = value` never
# matched at all: it fell through to ordinary expression parsing
# instead, which happily compiled `x[a][b]` as a read (parse_precedence's
# own postfix-chaining loop already handles repeated `[...]`/`.`
# unconditionally), leaving the trailing `= value` as unconsumed tokens
# -- "expected newline after expression". A previously-undiscovered
# gap, not a documented cut (see indexed_assignment_to_ivar_and_cvar.di
# for the sibling bug this one's shaped just like: same file, same
# functions, one level short of a full chain). Found building
# packages/graphql's Dataloader, which needed `context["calls"][0] =
# context["calls"][0] + 1`.
require "../../lib/minitest"

class Box
  def initialize()
    @data = {"x": {"y": {"z": 1}}}
    @grid = [[0, 0], [0, 0]]
  end
  def deep() = @data
  def set_deep(value)
    @data["x"]["y"]["z"] = value
  end
  def grid() = @grid
  def set_cell(row, col, value)
    @grid[row][col] = value
  end
end

class Counter
  def initialize()
    @shared = {"totals": {}}
  end
  def bump(key)
    @shared["totals"][key] = self.count(key) + 1
  end
  def count(key)
    value = @shared["totals"][key]
    if value == nil then 0 else value end
  end
end

class Registry
  def self.reset()
    @@nested = {"counts": {}}
  end
  def bump(key)
    @@nested["counts"][key] = self.count(key) + 1
  end
  def count(key)
    value = @@nested["counts"][key]
    if value == nil then 0 else value end
  end
end

def run_tests()
  def test_two_level_hash_chain()
    box = Box.new()
    box.set_deep(42)
    Minitest.assert_equal(42, box.deep()["x"]["y"]["z"])
  end

  def test_two_level_array_chain()
    box = Box.new()
    box.set_cell(1, 0, 7)
    Minitest.assert_equal(0, box.grid()[0][0])
    Minitest.assert_equal(7, box.grid()[1][0])
    Minitest.assert_equal(0, box.grid()[1][1])
  end

  def test_three_level_local_chain()
    a = {"p": {"q": {"r": 1}}}
    a["p"]["q"]["r"] = 99
    Minitest.assert_equal(99, a["p"]["q"]["r"])
  end

  def test_chained_index_assignment_with_indexed_rhs()
    counter = Counter.new()
    counter.bump("x")
    counter.bump("x")
    counter.bump("x")
    Minitest.assert_equal(3, counter.count("x"))
  end

  def test_cvar_chained_index_assignment_shared_across_instances()
    Registry.reset()
    r1 = Registry.new()
    r2 = Registry.new()
    r1.bump("x")
    r2.bump("x")
    r1.bump("x")
    Minitest.assert_equal(3, r2.count("x"))
  end

  suite = Minitest.new()
  suite.test("two-level ivar Hash index-assignment chain", test_two_level_hash_chain)
  suite.test("two-level ivar Array index-assignment chain", test_two_level_array_chain)
  suite.test("three-level local index-assignment chain", test_three_level_local_chain)
  suite.test("chained index assignment with indexed RHS", test_chained_index_assignment_with_indexed_rhs)
  suite.test("cvar chained index assignment shared across instances",
             test_cvar_chained_index_assignment_shared_across_instances)
  suite.run!()
end

run_tests()
