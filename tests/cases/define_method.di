# ClassName.define_method(name, callable) -- redefine_method's
# add-a-new-slot counterpart, see docs/syntax.md and docs/design.md.
require "../../lib/minitest"

class Greeter
  def initialize(name)
    @name = name
  end
  def self.greet_factory()
    def greet()
      "hello, #{@name}"
    end
    greet
  end
end

class Adder
  def self.add_factory()
    def add(a, b)
      a + b
    end
    add
  end
end

class Other
  def self.other_factory()
    def other_method()
      "wrong class"
    end
    other_method
  end
end

class Capturing
  def self.capturing_factory(x)
    def captured()
      x
    end
    captured
  end
end

def run_tests()
  def test_defines_a_genuinely_new_method()
    g = Greeter.new("Ada")
    Greeter.define_method("greet", Greeter.greet_factory())
    Minitest.assert_equal("hello, Ada", g.greet())
  end

  def test_dispatches_for_instances_constructed_before_the_call()
    before = Greeter.new("Before")
    Greeter.define_method("salutation", Greeter.greet_factory())
    Minitest.assert_equal("hello, Before", before.salutation())
    after = Greeter.new("After")
    Minitest.assert_equal("hello, After", after.salutation())
  end

  def test_new_method_takes_the_callables_own_arity()
    Adder.define_method("add", Adder.add_factory())
    adder = Adder.new()
    Minitest.assert_equal(7, adder.add(3, 4))
  end

  def test_rejects_a_name_that_already_exists()
    Greeter.define_method("already_here", Greeter.greet_factory())
    message = nil
    begin
      Greeter.define_method("already_here", Greeter.greet_factory())
    rescue error: TypeError
      message = error.message()
    end
    Minitest.assert_equal(
      "class 'Greeter' already has a method 'already_here' -- use redefine_method instead",
      message)
  end

  def test_rejects_a_capturing_callable()
    message = nil
    begin
      Capturing.define_method("captured_method", Capturing.capturing_factory(5))
    rescue error: TypeError
      message = error.message()
    end
    Minitest.assert_equal("define_method callable must not capture any variables", message)
  end

  def test_rejects_a_callable_from_a_different_class()
    message = nil
    begin
      Greeter.define_method("borrowed", Other.other_factory())
    rescue error: TypeError
      message = error.message()
    end
    Minitest.assert_equal("define_method callable must be a method of 'Greeter'", message)
  end

  def test_rejects_a_non_string_name()
    message = nil
    begin
      Greeter.define_method(1, Greeter.greet_factory())
    rescue error: TypeError
      message = error.message()
    end
    Minitest.assert_equal("define_method name must be a String", message)
  end

  suite = Minitest.new()
  suite.test("defines a genuinely new method", test_defines_a_genuinely_new_method)
  suite.test("dispatches for instances constructed before the call",
             test_dispatches_for_instances_constructed_before_the_call)
  suite.test("new method takes the callable's own arity",
             test_new_method_takes_the_callables_own_arity)
  suite.test("rejects a name that already exists", test_rejects_a_name_that_already_exists)
  suite.test("rejects a capturing callable", test_rejects_a_capturing_callable)
  suite.test("rejects a callable from a different class",
             test_rejects_a_callable_from_a_different_class)
  suite.test("rejects a non-String name", test_rejects_a_non_string_name)
  suite.run!()
end

run_tests()
