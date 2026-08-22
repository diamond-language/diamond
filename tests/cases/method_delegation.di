# Explicit-arity method delegation -- see docs/roadmap.md and
# docs/syntax.md. `delegate name(params), to: @ivar` compiles into an
# ordinary forwarding method (as if the source had literally been
# `def name(params) @ivar.name(params) end`), not a parallel runtime
# dispatch mechanism -- this suite's real point is confirming that claim:
# the generated method participates in inheritance/override/super,
# respond_to?, and redefine_method exactly like a hand-written one would.
#
# The four compile-time rejection paths (non-instance-variable target,
# `module_function` + delegate, delegating to an already-defined method
# name, and calling a generated method with the wrong arity) were verified
# directly against the built CLI rather than encoded here: they're compile
# errors or ordinary runtime arity errors, and this repo's only two
# automated test shapes are a stdout diff and this file's own Minitest
# exit-code suite, neither of which fits "compiling this file should fail
# with exactly this diagnostic" -- tests/parser_error_cases/ exists for a
# different purpose (parser_diff.sh's native/self-hosted differential
# comparison) and doesn't apply here, since this feature is deliberately
# native-only.
require "../../lib/minitest"

class Owner
  def initialize(name)
    @name = name
  end
  def owner_name()
    @name
  end
end

class Billing
  def charge(amount)
    "real charge #{amount}"
  end
end

class Account
  def initialize(owner, billing)
    @owner = owner
    @billing = billing
  end
  delegate owner_name(), to: @owner
  delegate charge(amount), to: @billing

  def self.charge_factory()
    def charge(amount)
      "overridden #{amount}"
    end
    charge
  end
end

class FreeAccount < Account
  def charge(amount)
    "free: " + super(amount)
  end
end

class Logger
  def log(message)
    "logged: #{message}"
  end
end

module Loggable
  delegate log(message), to: @logger
end

class Service
  include Loggable
  def initialize(logger)
    @logger = logger
  end
end

def run_tests()
  def test_zero_arg_class_delegation()
    a = Account.new(Owner.new("Ada"), Billing.new())
    Minitest.assert_equal("Ada", a.owner_name())
  end

  def test_class_delegation_with_argument()
    a = Account.new(Owner.new("Ada"), Billing.new())
    Minitest.assert_equal("real charge 10", a.charge(10))
  end

  def test_module_delegation_via_include()
    service = Service.new(Logger.new())
    Minitest.assert_equal("logged: hi", service.log("hi"))
  end

  def test_respond_to_sees_generated_method()
    a = Account.new(Owner.new("Ada"), Billing.new())
    Minitest.assert_equal(true, a.respond_to?(:charge))
    Minitest.assert_equal(true, a.respond_to?(:owner_name))
  end

  def test_subclass_override_and_super_reach_generated_method()
    f = FreeAccount.new(Owner.new("Ada"), Billing.new())
    Minitest.assert_equal("free: real charge 10", f.charge(10))
  end

  def test_redefine_method_repoints_generated_method()
    a = Account.new(Owner.new("Ada"), Billing.new())
    Minitest.assert_equal("real charge 10", a.charge(10))
    Account.redefine_method("charge", Account.charge_factory())
    Minitest.assert_equal("overridden 99", a.charge(99))
  end

  suite = Minitest.new()
  suite.test("zero-arg class delegation") do
    test_zero_arg_class_delegation()
  end
  suite.test("class delegation with an argument") do
    test_class_delegation_with_argument()
  end
  suite.test("module delegation via include") do
    test_module_delegation_via_include()
  end
  suite.test("respond_to? sees the generated method") do
    test_respond_to_sees_generated_method()
  end
  suite.test("subclass override + super reach the generated method") do
    test_subclass_override_and_super_reach_generated_method()
  end
  suite.test("redefine_method repoints the generated method") do
    test_redefine_method_repoints_generated_method()
  end
  suite.run!()
end

run_tests()
