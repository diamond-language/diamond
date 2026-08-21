# Indexed assignment (`x[key] = value`) previously only recognized a
# plain local variable as the receiver -- @ivar[key] = value and
# @@cvar[key] = value both failed to parse ("expected newline after
# expression"), not because of anything about the value being assigned,
# but because index_assignment_ahead/compile_index_assignment only ever
# checked for DIAMOND_TOKEN_IDENTIFIER. This was a documented, known gap
# (docs/syntax.md's own Comparable/class-variable section used to say
# "isn't supported yet" for exactly this), not a hidden bug. Found writing
# ActiveRecord::DirtyAttributes, which needs exactly this (@current[key]
# = value).
require "../../lib/minitest"

class Box
  def initialize()
    @data = {}
    @list = [0, 0, 0]
  end
  def set(key, value)
    @data[key] = value
  end
  def get(key) = @data[key]
  def set_index(i, value)
    @list[i] = value
  end
  def list() = @list
end

class Counter
  def initialize()
    @shared = {}
  end
  def bump(key)
    @shared[key] = self.count(key) + 1
  end
  def count(key)
    value = @shared[key]
    if value == nil then 0 else value end
  end
end

class Registry
  def self.reset()
    @@counts = {}
  end
  def bump(key)
    @@counts[key] = self.count(key) + 1
  end
  def count(key)
    value = @@counts[key]
    if value == nil then 0 else value end
  end
end

def run_tests()
  def test_ivar_hash_index_assignment()
    box = Box.new()
    box.set("a", 5)
    Minitest.assert_equal(5, box.get("a"))
  end

  def test_ivar_array_index_assignment()
    box = Box.new()
    box.set_index(1, 99)
    Minitest.assert_equal(0, box.list()[0])
    Minitest.assert_equal(99, box.list()[1])
    Minitest.assert_equal(0, box.list()[2])
  end

  def test_ivar_index_assignment_with_indexed_rhs()
    counter = Counter.new()
    counter.bump("x")
    counter.bump("x")
    counter.bump("x")
    Minitest.assert_equal(3, counter.count("x"))
  end

  def test_cvar_hash_index_assignment_shared_across_instances()
    Registry.reset()
    r1 = Registry.new()
    r2 = Registry.new()
    r1.bump("x")
    r2.bump("x")
    r1.bump("x")
    Minitest.assert_equal(3, r2.count("x"))
  end

  suite = Minitest.new()
  suite.test("ivar Hash index assignment", test_ivar_hash_index_assignment)
  suite.test("ivar Array index assignment", test_ivar_array_index_assignment)
  suite.test("ivar index assignment with indexed RHS", test_ivar_index_assignment_with_indexed_rhs)
  suite.test("cvar Hash index assignment shared across instances",
             test_cvar_hash_index_assignment_shared_across_instances)
  suite.run!()
end

run_tests()
