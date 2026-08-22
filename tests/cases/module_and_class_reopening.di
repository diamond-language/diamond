# Module and class reopening (see docs/roadmap.md and docs/syntax.md's
# "Classes" section): a second `module Foo ... end`/`class Foo ... end`
# for a name that already exists now adds to it instead of erroring --
# the real gap that blocked splitting packages/arel and
# packages/active_record into one file per class, unrelated to
# (but built on top of) this session's earlier declaration-discovery
# pass. Genuine cross-file coverage lives in tests/multifile/ (this file
# tests the same compiler mechanism, which doesn't care whether a
# "second declaration" comes from a different file or just later in the
# same one -- require-flattening makes them identical by the time the
# compiler ever sees the source).
require "../../lib/minitest"

module Reopened
  class Inner
    def from_first_block() = "first"
  end
end

module Reopened
  class Inner
    def from_second_block() = "second"
  end
  class SecondClass
    def hi() = "second-class"
  end
end

class Widget
  def initialize(name)
    @name = name
  end
  def name() = @name
end

class Widget
  def rename(new_name)
    @name = new_name
  end
end

class Widget
  def shout() = @name.upcase()
end

class Based
  def initialize()
    @tag = "base-tag"
  end
  def tag() = @tag
end

class Extended < Based
  def initialize()
    super()
    @extra = "extra"
  end
  def extra() = @extra
end

# Reopened without restating `< Based` -- the superclass established by
# the first declaration must survive untouched.
class Extended
  def combined() = @tag + "/" + @extra
end

# Reopened *restating* the same superclass -- a validated no-op, and
# must not stomp @tag/@extra already accumulated above.
class Extended < Based
  def combined_again() = self.combined()
end

interface Nameable
  def name()
  def rename(new_name)
end

def run_tests()
  def test_module_reopened_across_two_blocks()
    Minitest.assert_equal("first", Reopened::Inner.new().from_first_block())
    Minitest.assert_equal("second", Reopened::Inner.new().from_second_block())
    Minitest.assert_equal("second-class", Reopened::SecondClass.new().hi())
  end

  def test_class_reopened_three_times_merges_methods_and_fields()
    w = Widget.new("ada")
    Minitest.assert_equal("ada", w.name())
    w.rename("grace")
    Minitest.assert_equal("grace", w.name())
    Minitest.assert_equal("GRACE", w.shout())
  end

  def test_reopen_without_superclass_clause_preserves_it()
    e = Extended.new()
    Minitest.assert_equal("base-tag", e.tag())
    Minitest.assert_equal("extra", e.extra())
    Minitest.assert_equal("base-tag/extra", e.combined())
  end

  def test_reopen_restating_same_superclass_is_a_no_op()
    e = Extended.new()
    Minitest.assert_equal("base-tag/extra", e.combined_again())
  end

  def test_interface_satisfaction_still_correct_for_a_reopened_class()
    w = Widget.new("ada")
    Minitest.assert_equal(true, w is Nameable)
  end

  suite = Minitest.new()
  suite.test("module reopened across two blocks merges classes into it") do
    test_module_reopened_across_two_blocks()
  end
  suite.test("class reopened three times merges methods and fields") do
    test_class_reopened_three_times_merges_methods_and_fields()
  end
  suite.test("reopen without a superclass clause preserves the existing one") do
    test_reopen_without_superclass_clause_preserves_it()
  end
  suite.test("reopen restating the same superclass is a validated no-op") do
    test_reopen_restating_same_superclass_is_a_no_op()
  end
  suite.test("interface satisfaction is still correct for a reopened class") do
    test_interface_satisfaction_still_correct_for_a_reopened_class()
  end
  suite.run!()
end

run_tests()
