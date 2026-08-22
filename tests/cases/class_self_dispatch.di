# Virtual dispatch for def self.x class methods -- see docs/syntax.md and
# docs/design.md. `self` is usable inside a class-owned singleton method
# (holding the actual receiver class, which may differ from the class the
# method is lexically defined in), and self.foo(...) inside one dispatches
# by name against that class, walking its superclass chain -- unlike a
# bare foo() call, which stays static/unchanged.
require "../../lib/minitest"

class Model
  def self.table_name()
    "model_default"
  end
  def self.describe()
    self.table_name()
  end
  def self.missing()
    self.does_not_exist()
  end
end

class Author < Model
  def self.table_name()
    "authors"
  end
end

class Book < Model
end

class Grandparent
  def self.value()
    "grandparent"
  end
  def self.describe()
    self.value()
  end
end
class Parent < Grandparent
end
class Child < Parent
  def self.value()
    "child"
  end
end

class Calc
  def self.add(a, b)
    a + b
  end
  def self.compute(x, y)
    self.add(x, y) * 2
  end
end
class BigCalc < Calc
  def self.add(a, b)
    a + b + 100
  end
end

class Arity
  def self.needs_two(a, b) = a + b
  def self.bad_call()
    self.needs_two(1)
  end
end

class Widget
  def self.identity()
    self
  end
end

module Utility
  def self.helper()
    "helper called"
  end
end

def run_tests()
  def test_inherited_method_reaches_subclass_override()
    Minitest.assert_equal("authors", Author.describe())
  end

  def test_inherited_method_falls_back_to_ancestor()
    Minitest.assert_equal("model_default", Book.describe())
  end

  def test_called_directly_on_the_defining_class()
    Minitest.assert_equal("model_default", Model.describe())
  end

  def test_walks_multiple_ancestor_levels()
    Minitest.assert_equal("child", Child.describe())
    Minitest.assert_equal("grandparent", Parent.describe())
  end

  def test_arguments_pass_through_to_the_dispatched_method()
    Minitest.assert_equal(206, BigCalc.compute(1, 2))
    Minitest.assert_equal(6, Calc.compute(1, 2))
  end

  def test_undefined_self_method_raises_clearly()
    message = nil
    begin
      Model.missing()
    rescue error: TypeError
      message = error.message()
    end
    # A trailing "\n  at missing:line:col" is normal Diamond behavior for
    # any exception that crosses a function-call boundary (Model.missing()
    # is a real call, unlike define_method/redefine_method's own errors,
    # which raise inline with no such frame to cross) -- check the prefix,
    # not an exact match.
    Minitest.assert_equal(true,
      message.start_with?("undefined class singleton method 'does_not_exist' for Model"))
  end

  def test_arity_mismatch_on_self_dispatch_raises()
    message = nil
    begin
      Arity.bad_call()
    rescue error: ArgumentError
      message = error.message() != nil
    end
    Minitest.assert_equal(true, message)
  end

  def test_bare_self_is_a_usable_class_value()
    Minitest.assert_equal(true, Widget.identity() == Widget.identity())
  end

  def test_module_namespace_singletons_are_unaffected()
    Minitest.assert_equal("helper called", Utility.helper())
  end

  suite = Minitest.new()
  suite.test("inherited method reaches subclass override",
             test_inherited_method_reaches_subclass_override)
  suite.test("inherited method falls back to ancestor",
             test_inherited_method_falls_back_to_ancestor)
  suite.test("called directly on the defining class",
             test_called_directly_on_the_defining_class)
  suite.test("walks multiple ancestor levels", test_walks_multiple_ancestor_levels)
  suite.test("arguments pass through to the dispatched method",
             test_arguments_pass_through_to_the_dispatched_method)
  suite.test("undefined self method raises clearly", test_undefined_self_method_raises_clearly)
  suite.test("arity mismatch on self dispatch raises",
             test_arity_mismatch_on_self_dispatch_raises)
  suite.test("bare self is a usable class value", test_bare_self_is_a_usable_class_value)
  suite.test("module namespace singletons are unaffected",
             test_module_namespace_singletons_are_unaffected)
  suite.run!()
end

run_tests()
