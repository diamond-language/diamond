# Qualified Module::Class resolution in three grammar positions that
# previously only accepted a single bare identifier token: superclass
# declarations, `is` type checks, and `rescue` clause types. Found while
# restructuring Arel/active_record under real module namespaces --
# external code needs to subclass a nested class (e.g. a third-party Arel
# dialect visitor extending Arel::Visitor from its own file), which a bare
# name can never do since Diamond has no module-reopening.
require "../../lib/minitest"

module Shapes
  class Base
    def initialize(name: String)
      @name = name
    end
    def name() = @name
  end

  class Sibling < Base
    def loud() = self.name().upcase()
  end

  # Same bare name as the unrelated top-level `class Table` below --
  # a bare `is`/type-annotation reference to `Table` from code lexically
  # inside this module must resolve to *this* class, not the top-level
  # one, even though both exist in the same compiled program.
  class Table
    def initialize(name: String)
      @name = name
    end
    def name() = @name
  end

  def self.wrap(value)
    if value is Table then "shapes table: #{value.name()}" else "not a shapes table" end
  end

  def self.describe(table: Table) = "described: #{table.name()}"
end

class External < Shapes::Base
  def loud() = self.name().upcase()
end

# Unrelated top-level class that happens to share a bare name with
# Shapes::Table above -- this collision used to make a bare `is`/type
# annotation reference *inside* module Shapes resolve to this class
# instead (find_class_qualified_or_scoped tried the bare/global name
# before the module-qualified one), silently corrupting values instead
# of raising a resolution error.
class Table
  def initialize(name: String)
    @name = name
  end
  def name() = @name
end

class ShapeError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

module Errors
  class Nested < StandardError
    attr_reader message: String
    def initialize(message: String)
      @message = message
    end
  end
end

def run_tests()
  def test_bare_sibling_inheritance_still_works()
    Minitest.assert_equal("X", Shapes::Sibling.new("x").loud())
  end

  def test_qualified_external_inheritance()
    Minitest.assert_equal("HI", External.new("hi").loud())
  end

  def test_bare_is_check_inside_module()
    value = Shapes::Sibling.new("y")
    result = if value is Shapes::Base then "matched" else "no match" end
    Minitest.assert_equal("matched", result)
  end

  def test_qualified_is_check_from_outside()
    value = External.new("z")
    result = if value is Shapes::Base then "matched" else "no match" end
    Minitest.assert_equal("matched", result)
  end

  def test_bare_rescue_type_still_works()
    caught = false
    begin
      raise ShapeError.new("plain")
    rescue e: ShapeError
      caught = true
      Minitest.assert_equal("plain", e.message())
    end
    Minitest.assert_equal(true, caught)
  end

  def test_qualified_rescue_type()
    caught = false
    begin
      raise Errors::Nested.new("nested")
    rescue e: Errors::Nested
      caught = true
      Minitest.assert_equal("nested", e.message())
    end
    Minitest.assert_equal(true, caught)
  end

  def test_bare_is_check_prefers_own_module_over_collision()
    Minitest.assert_equal("shapes table: t1", Shapes.wrap(Shapes::Table.new("t1")))
    Minitest.assert_equal("not a shapes table", Shapes.wrap(Table.new("t2")))
  end

  def test_bare_type_annotation_prefers_own_module_over_collision()
    Minitest.assert_equal("described: t3", Shapes.describe(Shapes::Table.new("t3")))
  end

  suite = Minitest.new()
  suite.test("bare sibling inheritance", test_bare_sibling_inheritance_still_works)
  suite.test("qualified external inheritance", test_qualified_external_inheritance)
  suite.test("bare is check inside module", test_bare_is_check_inside_module)
  suite.test("qualified is check from outside", test_qualified_is_check_from_outside)
  suite.test("bare rescue type", test_bare_rescue_type_still_works)
  suite.test("qualified rescue type", test_qualified_rescue_type)
  suite.test("bare is check prefers own module over same-named top-level class",
    test_bare_is_check_prefers_own_module_over_collision)
  suite.test("bare type annotation prefers own module over same-named top-level class",
    test_bare_type_annotation_prefers_own_module_over_collision)
  suite.run!()
end

run_tests()
