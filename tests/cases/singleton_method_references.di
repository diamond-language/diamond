# `ClassName.method`/`ModuleName.method` -- a bare reference to a
# class-owned `self.` or module singleton method, with no call following,
# is a Callable value: a small synthesized zero-capture wrapper function
# forwarding into the target via the exact same compiled call shape an
# ordinary `ClassName.method(args)` call site already produces. See
# docs/design.md's "Bare singleton method references" section.
#
# The two compile-time rejections (a variadic target, a generic target)
# were verified directly against the built CLI rather than encoded here
# -- same reasoning tests/cases/method_delegation.di's own header
# already gives: a compile error doesn't fit this file's two available
# shapes (a stdout diff or this file's own Minitest exit-code suite).
require "../../lib/minitest"
require "../../packages/dials/lib/dials"

class Calculator
  def self.zero_args()
    "no args"
  end
  def self.double(a)
    a * 2
  end
  def self.add(a, b)
    a + b
  end
end

module Greeting
  module_function
  def self.shout(name)
    "#{name.upcase()}!"
  end
end

class DoubleController
  def self.show(request, context, params)
    Dials::Response.text(200, params["n"].to_i() * 2)
  end
end

def call_zero(f: Callable[0]) = f()
def call_one(f: Callable[1], a) = f(a)
def call_two(f: Callable[2], a, b) = f(a, b)

def run_tests()
  def test_zero_arg_class_method_reference()
    Minitest.assert_equal("no args", call_zero(Calculator.zero_args))
  end

  def test_one_arg_class_method_reference()
    Minitest.assert_equal(10, call_one(Calculator.double, 5))
  end

  def test_two_arg_class_method_reference()
    Minitest.assert_equal(7, call_two(Calculator.add, 3, 4))
  end

  def test_module_singleton_function_reference()
    Minitest.assert_equal("ADA!", call_one(Greeting.shout, "ada"))
  end

  def test_two_reference_sites_to_the_same_method_are_independent()
    f1 = Calculator.double
    f2 = Calculator.double
    Minitest.assert_equal(20, f1(10))
    Minitest.assert_equal(40, f2(20))
    Minitest.assert_equal(60, f1(30))
  end

  def test_callable_n_type_checks_a_reference()
    Minitest.assert_equal(7, call_two(Calculator.add, 3, 4))
  end

  def test_dials_router_accepts_a_bare_reference_directly()
    router = Dials::Router.new()
    router.get("/double/:n", DoubleController.show)
    response = router.dispatch({"path": "/double/21", "method": "GET", "body": ""}, {})
    Minitest.assert_equal(42, response[2])
  end

  suite = Minitest.new()
  suite.test("zero-arg class method reference", test_zero_arg_class_method_reference)
  suite.test("one-arg class method reference", test_one_arg_class_method_reference)
  suite.test("two-arg class method reference", test_two_arg_class_method_reference)
  suite.test("module singleton function reference", test_module_singleton_function_reference)
  suite.test("two reference sites to the same method are independent",
             test_two_reference_sites_to_the_same_method_are_independent)
  suite.test("Callable[N] type-checks a reference", test_callable_n_type_checks_a_reference)
  suite.test("Dials::Router accepts a bare reference directly",
             test_dials_router_accepts_a_bare_reference_directly)
  suite.run!()
end

run_tests()
