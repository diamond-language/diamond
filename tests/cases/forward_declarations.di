# diamond_compile's own declaration-discovery pass (see docs/roadmap.md)
# -- a class/module/interface, and a type annotation naming one, can now
# be referenced before its own declaration is textually reached later in
# the same source. The mechanism: the whole source compiles twice, once
# in a throwaway "discovery" pass that tolerates an unresolved forward
# reference just long enough to walk the entire file and fully register
# every declaration (names, fields, methods -- including singleton
# methods), then again for real with everything already known. What this
# suite is really pinning down: the trickiest bug found while building
# this (an interface's own recorded method arity silently doubling,
# because "claiming" a declaration pass 1 already registered reset the
# method *count* but not the stale contents already sitting in the
# methods array) reproduced exactly on a one-parameter interface method,
# so that shape gets its own dedicated coverage below, not just the
# zero-parameter shape that happened to keep working throughout.
require "../../lib/minitest"

# `Ping.new()` referenced from Pong's own method body, before `class
# Ping` is textually reached -- the exact shape that motivated this
# feature (ActiveRecord::Relation.new(...) called from Repository#relation,
# with Relation declared later in the file).
class Pong
  def make_ping() = Ping.new()
end

class Ping
  def initialize()
    @marker = "ping"
  end
  def marker() = @marker
end

# A class inside a module referencing a sibling class declared later in
# the same module -- the real shape active_record's own
# Repository/Relation classes are in.
module Nested
  class First
    def make_second() = Second.new()
  end
  class Second
    def initialize()
      @value = 42
    end
    def value() = @value
  end
end

# `OtherSide.value()` -- a class-owned singleton method call (not `.new`)
# on a class declared later in the file. This is the harder case: unlike
# construction (which only ever needs the callee's index), a singleton
# call is arity-checked against the callee's actual signature at compile
# time, which requires the callee to be *fully* compiled already, not
# just registered by name -- and it's the case
# packages/active_record's own `wire_*` class-variable-indirection
# pattern (tests/cases/active_record_model.di) exists to work
# around, for exactly this reason.
class ThisSide
  def self.call_other() = OtherSide.value()
end

class OtherSide
  def self.value() = 99
end

# A type annotation naming a class declared later in the file.
def wrap(x: LaterAnnotated) = x

class LaterAnnotated
  def initialize()
    @tag = "later"
  end
  def tag() = @tag
end

# The bug this whole suite exists to pin down: an interface method that
# takes at least one parameter, required by a class declared *after* it
# gets forward-referenced *from inside that same interface's own
# declaration point* by nothing else in the file needing it early -- but
# every class in the program still goes through the discovery/claim cycle
# regardless of whether it personally needed a forward reference, so this
# reproduces the arity-doubling bug even though nothing here looks like a
# forward reference at all.
interface Nameable
  def rename(new_name) -> String
end

class Widget
  def initialize(name)
    @name = name
  end
  def rename(new_name) -> String
    @name = new_name
    @name
  end
end

def run_tests()
  def test_construction_forward_reference()
    Minitest.assert_equal("ping", Pong.new().make_ping().marker())
  end

  def test_construction_forward_reference_inside_a_module()
    Minitest.assert_equal(42, Nested::First.new().make_second().value())
  end

  def test_singleton_call_forward_reference()
    Minitest.assert_equal(99, ThisSide.call_other())
  end

  def test_type_annotation_forward_reference()
    Minitest.assert_equal("later", wrap(LaterAnnotated.new()).tag())
  end

  def test_interface_satisfaction_with_a_parameterized_method()
    w = Widget.new("old")
    Minitest.assert_equal(true, w is Nameable)
    Minitest.assert_equal("new", w.rename("new"))
  end

  suite = Minitest.new()
  suite.test("construction forward reference (Class.new before its own declaration)") do
    test_construction_forward_reference()
  end
  suite.test("construction forward reference inside a module") do
    test_construction_forward_reference_inside_a_module()
  end
  suite.test("singleton-method-call forward reference (the wire_* case)") do
    test_singleton_call_forward_reference()
  end
  suite.test("type annotation forward reference") do
    test_type_annotation_forward_reference()
  end
  suite.test("interface satisfaction still correct for a parameterized method") do
    test_interface_satisfaction_with_a_parameterized_method()
  end
  suite.run!()
end

run_tests()
