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
end

class External < Shapes::Base
  def loud() = self.name().upcase()
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

  suite = Minitest.new()
  suite.test("bare sibling inheritance", test_bare_sibling_inheritance_still_works)
  suite.test("qualified external inheritance", test_qualified_external_inheritance)
  suite.test("bare is check inside module", test_bare_is_check_inside_module)
  suite.test("qualified is check from outside", test_qualified_is_check_from_outside)
  suite.test("bare rescue type", test_bare_rescue_type_still_works)
  suite.test("qualified rescue type", test_qualified_rescue_type)
  suite.run!()
end

run_tests()
